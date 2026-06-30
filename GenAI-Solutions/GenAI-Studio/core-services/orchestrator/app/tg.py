#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Text-Generation routes and upstream resolution helpers.
# ---------------------------------------------------------------------

import asyncio
import os
from typing import Optional

import httpx
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from .common import (
    _error_body,
    _is_openai_base,
    _make_v1_url,
    _ms_since,
    _normalize_base_url,
    _now,
    _proxy_json_post,
    _proxy_raw_request,
)
from .context import (
    OPENAI_API_KEY,
    OPENAI_BASE_URL,
    TG_DIRECT_MODEL_ID,
    TG_ORCHESTRATOR_MODEL_ID,
    TG_URL,
    http_client_default,
    http_client_stream,
)
from .npu_arbiter import NpuBusyError, acquire_npu_slot, acquire_npu_slot_handle

router = APIRouter()

TG_INTERNAL_API_KEY = os.getenv("TG_INTERNAL_API_KEY", "tg-internal-placeholder-key").strip()
TG_BUSY_RETRY_AFTER_SEC = os.getenv("TG_BUSY_RETRY_AFTER_SEC", "2").strip() or "2"
try:
    TG_SINGLE_FLIGHT_TIMEOUT_SEC = float(os.getenv("TG_SINGLE_FLIGHT_TIMEOUT_SEC", "30"))
except Exception:
    TG_SINGLE_FLIGHT_TIMEOUT_SEC = 30.0

_tg_single_flight_lock = asyncio.Lock()


def _busy_response(message: str = "Text model is busy. Retry after current request completes.") -> JSONResponse:
    """Build consistent busy response payload for single-flight contention."""
    resp = JSONResponse(
        content={"error": {"message": message, "type": "server_error", "code": "model_busy"}},
        status_code=503,
    )
    resp.headers["Retry-After"] = TG_BUSY_RETRY_AFTER_SEC
    return resp


async def _acquire_tg_single_flight_or_busy() -> Optional[JSONResponse]:
    """Acquire global TG slot or return model_busy when wait timeout expires."""
    try:
        await asyncio.wait_for(_tg_single_flight_lock.acquire(), timeout=TG_SINGLE_FLIGHT_TIMEOUT_SEC)
        return None
    except asyncio.TimeoutError:
        return _busy_response()


def _make_tg_single_flight_releaser():
    """Build idempotent releaser for a successfully acquired single-flight slot."""
    released = False

    def _release() -> None:
        nonlocal released
        if released:
            return
        released = True
        try:
            _tg_single_flight_lock.release()
        except RuntimeError:
            return

    return _release


def _resolve_tg_upstream(request: Request) -> tuple[Optional[str], dict, Optional[JSONResponse]]:
    """Resolve Text-Generation upstream base URL and auth headers for edge/cloud modes."""
    mode_raw = (
        request.headers.get("x-tg-mode")
        or request.headers.get("x-endpoint-mode")
        or os.getenv("TG_PROVIDER", "edge")
    )
    mode = str(mode_raw).strip().lower()
    base_hdr = request.headers.get("x-tg-base-url") or request.headers.get("x-endpoint-url") or ""
    incoming_auth = request.headers.get("authorization", "").strip()
    api_key = request.headers.get("x-api-key", "").strip()

    provider = "openai" if mode in ("cloud", "openai") else "edge"
    candidate_base = _normalize_base_url(base_hdr, OPENAI_BASE_URL if provider == "openai" else TG_URL)
    if _is_openai_base(candidate_base):
        provider = "openai"

    if provider == "openai":
        headers = {}
        if incoming_auth:
            headers["Authorization"] = incoming_auth
        elif api_key:
            headers["Authorization"] = f"Bearer {api_key}"
        elif OPENAI_API_KEY:
            headers["Authorization"] = f"Bearer {OPENAI_API_KEY}"
        else:
            return None, {}, JSONResponse(
                content=_error_body(
                    "OpenAI mode requires API key. Provide X-API-Key, Authorization header, or OPENAI_API_KEY.",
                    "authentication_error",
                ),
                status_code=401,
            )
        return candidate_base, headers, None

    headers = {}
    if incoming_auth:
        headers["Authorization"] = incoming_auth
    internal_api_key = request.headers.get("x-internal-api-key", "").strip() or TG_INTERNAL_API_KEY
    if internal_api_key:
        headers["X-Internal-API-Key"] = internal_api_key
    return candidate_base, headers, None


@router.get("/api/tg/models")
async def tg_models(request: Request):
    """Proxy model listing to the selected Text-Generation upstream."""
    base_url, headers, err = _resolve_tg_upstream(request)
    if err is not None:
        return err
    return await _proxy_raw_request(
        request,
        method="GET",
        upstream_url=_make_v1_url(base_url, "models"),
        client=http_client_stream,
        extra_headers=headers,
    )


@router.get("/api/tg/local-models")
async def tg_local_models(request: Request):
    """Return on-device local model inventory from internal TG endpoint."""
    base_url, headers, err = _resolve_tg_upstream(request)
    if err is not None:
        return err
    if not base_url or _is_openai_base(base_url):
        return JSONResponse(
            content=_error_body("Local model discovery is available only for On-Device mode.", "invalid_request_error"),
            status_code=400,
        )

    try:
        r = await http_client_default.get(
            _make_v1_url(base_url, "internal/models"),
            headers=headers,
            timeout=10.0,
        )
    except Exception as exc:
        return JSONResponse(
            content=_error_body(f"Failed to fetch local text models: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )

    try:
        body = r.json()
    except Exception:
        body = {"error": {"message": r.text[:400], "type": "upstream_error"}}
    return JSONResponse(content=body, status_code=r.status_code)


@router.post("/api/tg/load-model")
async def tg_load_model(request: Request):
    """Switch active on-device text model through TG internal load endpoint."""
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse(content=_error_body("Invalid JSON"), status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse(content=_error_body("JSON body must be an object"), status_code=400)

    base_url, headers, err = _resolve_tg_upstream(request)
    if err is not None:
        return err
    if not base_url or _is_openai_base(base_url):
        return JSONResponse(
            content=_error_body("Model switching is available only for On-Device mode.", "invalid_request_error"),
            status_code=400,
        )

    busy = await _acquire_tg_single_flight_or_busy()
    if busy is not None:
        return busy
    release_slot = _make_tg_single_flight_releaser()

    try:
        try:
            async with acquire_npu_slot("tg-load-model"):
                r = await http_client_default.post(
                    _make_v1_url(base_url, "internal/models/load"),
                    json=payload,
                    headers=headers,
                    timeout=120.0,
                )
        except NpuBusyError as exc:
            return _busy_response(str(exc))
        except Exception as exc:
            return JSONResponse(
                content=_error_body(f"Failed to switch model: {str(exc)[:300]}", "upstream_error"),
                status_code=503,
            )

        try:
            body = r.json()
        except Exception:
            body = {"error": {"message": r.text[:400], "type": "upstream_error"}}

        out = JSONResponse(content=body, status_code=r.status_code)
        retry_after = r.headers.get("Retry-After")
        if retry_after:
            out.headers["Retry-After"] = retry_after
        return out
    finally:
        release_slot()


@router.post("/api/tg/chat")
async def tg_chat(request: Request):
    """Handle chat completion requests with optional stream proxy and timing metadata."""
    t0 = _now()
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse(content=_error_body("Invalid JSON"), status_code=400)

    parse_ms = _ms_since(t0)
    if not isinstance(payload, dict):
        return JSONResponse(
            content=_error_body("JSON body must be an object"),
            status_code=400,
        )

    messages = payload.get("messages", [])
    stream = payload.get("stream", False)
    max_tokens = payload.get("max_tokens", 512)
    model = payload.get("model", TG_ORCHESTRATOR_MODEL_ID)
    if not isinstance(messages, list) or not messages:
        return JSONResponse(
            content={"error": {"message": "'messages' must be a non-empty array", "type": "invalid_request_error"}},
            status_code=400,
        )
    if not isinstance(stream, bool):
        stream = str(stream).lower() in ("1", "true", "yes")
    if not isinstance(model, str):
        return JSONResponse(
            content=_error_body("'model' must be a string"),
            status_code=400,
        )
    model = model.strip()
    if not model:
        return JSONResponse(
            content=_error_body("'model' must be a non-empty string"),
            status_code=400,
        )

    upstream_payload = dict(payload)
    upstream_payload["messages"] = messages
    upstream_payload["stream"] = stream
    upstream_payload["max_tokens"] = max_tokens
    upstream_payload["model"] = model

    base_url, routing_headers, routing_err = _resolve_tg_upstream(request)
    if routing_err is not None:
        return routing_err

    if base_url and not _is_openai_base(base_url):
        # If using orchestrator model, fetch active model from backend
        if model == TG_ORCHESTRATOR_MODEL_ID:
            try:
                r = await http_client_default.get(
                    _make_v1_url(base_url, "internal/models"),
                    headers=routing_headers,
                    timeout=10.0,
                )
                if r.status_code == 200:
                    body = r.json()
                    active_model_id = body.get("active_model_id", "")
                    if active_model_id:
                        upstream_payload["model"] = active_model_id
                    else:
                        return JSONResponse(
                            content=_error_body("Could not determine active model from backend"),
                            status_code=503,
                        )
                else:
                    return JSONResponse(
                        content=_error_body("Failed to fetch active model from backend"),
                        status_code=503,
                    )
            except Exception as exc:
                return JSONResponse(
                    content=_error_body(f"Failed to fetch active model: {str(exc)[:300]}"),
                    status_code=503,
                )
        # Otherwise, pass model through as-is (direct backend access)

    upstream_headers = {"Content-Type": "application/json", **routing_headers}
    upstream_chat_url = _make_v1_url(base_url, "chat/completions")

    busy = await _acquire_tg_single_flight_or_busy()
    if busy is not None:
        return busy
    release_slot = _make_tg_single_flight_releaser()

    if stream:
        try:
            npu_meta, release_npu = await acquire_npu_slot_handle("tg-chat-stream")
        except NpuBusyError as exc:
            release_slot()
            return _busy_response(str(exc))
        npu_wait_ms = float(npu_meta.get("wait_ms", 0.0))
        released = False

        def release_all() -> None:
            nonlocal released
            if released:
                return
            released = True
            release_npu()
            release_slot()

        stream_options = upstream_payload.get("stream_options")
        if not isinstance(stream_options, dict):
            stream_options = {}
        stream_options.setdefault("include_usage", True)
        upstream_payload["stream_options"] = stream_options

        t_up = _now()
        proxied = await _proxy_json_post(
            upstream_url=upstream_chat_url,
            payload=upstream_payload,
            headers=upstream_headers,
            client=http_client_stream,
            on_close=release_all,
        )
        if isinstance(proxied, JSONResponse):
            return proxied
        if hasattr(proxied, "headers"):
            proxied.headers["X-Parse-Ms"] = str(parse_ms)
            proxied.headers["X-Upstream-Setup-Ms"] = str(_ms_since(t_up))
            proxied.headers["X-Request-Setup-Ms"] = str(_ms_since(t0))
            proxied.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
        return proxied

    try:
        npu_wait_ms = 0.0
        try:
            async with acquire_npu_slot("tg-chat") as npu_meta:
                npu_wait_ms = float(npu_meta.get("wait_ms", 0.0))
                t_up = _now()
                resp = await http_client_stream.post(
                    upstream_chat_url,
                    json=upstream_payload,
                    headers=upstream_headers,
                )
                upstream_ms = _ms_since(t_up)
        except NpuBusyError as exc:
            return _busy_response(str(exc))
        except httpx.HTTPError as exc:
            return JSONResponse(
                content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
                status_code=503,
            )

        try:
            body = resp.json()
        except Exception:
            body = {"error": {"code": "upstream_error", "message": resp.text[:400]}}

        if isinstance(body, dict):
            body["x_orchestrator_timing"] = {
                "parse_ms": parse_ms,
                "npu_wait_ms": npu_wait_ms,
                "upstream_ms": upstream_ms,
                "request_total_ms": _ms_since(t0),
            }
        out = JSONResponse(content=body, status_code=resp.status_code)
        retry_after = resp.headers.get("Retry-After")
        if retry_after:
            out.headers["Retry-After"] = retry_after
        out.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
        return out
    finally:
        release_slot()


@router.post("/api/tg/reset")
async def tg_reset(request: Request):
    """Reset the Text-Generation runtime model/session state."""
    base_url, headers, err = _resolve_tg_upstream(request)
    if err is not None:
        return err
    if not base_url or _is_openai_base(base_url):
        return JSONResponse(
            content=_error_body("Model reset is available only for On-Device mode.", "invalid_request_error"),
            status_code=400,
        )

    busy = await _acquire_tg_single_flight_or_busy()
    if busy is not None:
        return busy
    release_slot = _make_tg_single_flight_releaser()

    try:
        try:
            async with acquire_npu_slot("tg-reset"):
                r = await http_client_default.post(f"{base_url}/reset_model", headers=headers, timeout=60.0)
        except NpuBusyError as exc:
            return _busy_response(str(exc))
        try:
            body = r.json()
        except Exception:
            body = {"error": {"message": r.text[:400], "type": "upstream_error"}}
        out = JSONResponse(content=body, status_code=r.status_code)
        retry_after = r.headers.get("Retry-After")
        if retry_after:
            out.headers["Retry-After"] = retry_after
        return out
    except Exception as exc:
        return JSONResponse(
            content=_error_body(f"Failed to reset model: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )
    finally:
        release_slot()
