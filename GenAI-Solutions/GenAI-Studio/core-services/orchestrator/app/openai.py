#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# OpenAI-compatible routes and shared gateway endpoints.
# ---------------------------------------------------------------------

import asyncio
import json
import os

import httpx
from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, Response

from .common import (
    _error_body,
    _is_openai_base,
    _make_v1_url,
    _proxy_json_post,
    _proxy_raw_request,
    _response_from_upstream_with_close,
)
from .context import (
    I2T_URL,
    IMG_URL,
    OPENAI_API_KEY,
    OPENAI_BASE_URL,
    STT_URL,
    TG_DIRECT_MODEL_ID,
    TG_ORCHESTRATOR_MODEL_ID,
    TG_URL,
    TTS_URL,
    http_client_default,
    http_client_stream,
)
from .i2t import is_i2t_payload_wanted, wait_for_i2t_ready, _resolve_i2t_upstream, _extract_image_urls_from_messages
from .imagegen import _imagegen_headers
from .npu_arbiter import NpuBusyError, acquire_npu_slot_handle

router = APIRouter()
TG_INTERNAL_API_KEY = os.getenv("TG_INTERNAL_API_KEY", "").strip()
_STT_CANONICAL_MODEL = "whisper-1"
_STT_MODEL_ALIASES = {
    "whisper-tiny": _STT_CANONICAL_MODEL,
    "whisper-1": _STT_CANONICAL_MODEL,
    "gpt-4o-transcribe": _STT_CANONICAL_MODEL,
    "gpt-4o-mini-transcribe": _STT_CANONICAL_MODEL,
}


def _npu_busy_response(exc: NpuBusyError) -> JSONResponse:
    """Return standardized busy response when global inference lock times out."""
    resp = JSONResponse(content=_error_body(str(exc), "npu_busy"), status_code=503)
    resp.headers["Retry-After"] = "2"
    return resp


def _normalize_stt_model_name(model_value: object) -> str:
    if isinstance(model_value, str):
        requested = model_value.strip()
    else:
        requested = ""
    if not requested:
        return _STT_CANONICAL_MODEL
    return _STT_MODEL_ALIASES.get(requested, requested)


def _is_retryable_i2t_response(response: object) -> bool:
    """Return True when a proxied response represents transient I2T unavailability."""
    status_code = int(getattr(response, "status_code", 0) or 0)
    if status_code != 503:
        return False

    raw_body = getattr(response, "body", b"")
    if not isinstance(raw_body, (bytes, bytearray)) or not raw_body:
        return True
    try:
        payload = json.loads(raw_body.decode("utf-8", errors="replace"))
    except Exception:
        return True
    if not isinstance(payload, dict):
        return True

    err = payload.get("error")
    if not isinstance(err, dict):
        return True
    blob = " ".join(
        [
            str(err.get("code", "")),
            str(err.get("type", "")),
            str(err.get("message", "")),
        ]
    ).lower()
    if not blob.strip():
        return True
    retry_tokens = (
        "upstream_error",
        "upstream_unreachable",
        "connection",
        "all connection attempts failed",
        "failed to establish",
        "refused",
        "remote end closed",
    )
    return any(token in blob for token in retry_tokens)


async def _proxy_openai_stt_with_alias(
    request: Request,
    *,
    upstream_path: str,
    npu_owner: str,
):
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle(npu_owner)
    except NpuBusyError as exc:
        return _npu_busy_response(exc)

    # Prefer multipart parse path so model aliases can be normalized before
    # forwarding to STT backends that only accept canonical model ids.
    try:
        form = await request.form()
    except Exception:
        form = None

    if form is not None:
        file_part = form.get("file")
        if file_part is not None and hasattr(file_part, "read"):
            try:
                file_bytes = await file_part.read()
                filename = getattr(file_part, "filename", None) or "audio.wav"
                content_type = getattr(file_part, "content_type", None) or "application/octet-stream"

                form_data: dict[str, str] = {}
                for key, value in form.multi_items():
                    if key == "file":
                        continue
                    if hasattr(value, "read"):
                        continue
                    if key == "model":
                        continue
                    form_data[key] = str(value)
                form_data["model"] = _normalize_stt_model_name(form.get("model"))

                req = http_client_stream.build_request(
                    "POST",
                    f"{STT_URL}{upstream_path}",
                    files={"file": (filename, file_bytes, content_type)},
                    data=form_data,
                    params=request.query_params,
                )
                upstream = await http_client_stream.send(req, stream=True)
                response = await _response_from_upstream_with_close(upstream, on_close=release_npu)
                if hasattr(response, "headers"):
                    response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
                return response
            except httpx.HTTPError:
                # Fall back to raw proxy for consistent upstream-error shaping.
                pass
            except Exception:
                # Preserve compatibility by falling back to transparent proxying.
                pass

    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}{upstream_path}",
        client=http_client_stream,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


async def _fetch_models_list(url: str, headers: dict | None = None) -> list:
    """Fetch model list from a backend; return empty list on any failure."""
    try:
        r = await http_client_default.get(f"{url}/v1/models", headers=headers, timeout=8.0)
        if r.status_code != 200:
            return []
        body = r.json()
        data = body.get("data", [])
        return data if isinstance(data, list) else []
    except Exception:
        return []


@router.get("/v1/models")
async def openai_models(request: Request):
    """Merge models across TG/I2T/STT/TTS/ImageGen into OpenAI-compatible list."""
    img_headers = _imagegen_headers(request)
    results = await asyncio.gather(
        _fetch_models_list(TG_URL),
        _fetch_models_list(I2T_URL),
        _fetch_models_list(STT_URL),
        _fetch_models_list(TTS_URL),
        _fetch_models_list(IMG_URL, headers=img_headers if img_headers else None),
    )
    merged = []
    seen_ids = set()
    for items in results:
        for item in items:
            if not isinstance(item, dict):
                continue
            model_id = str(item.get("id", "")).strip()
            if not model_id or model_id in seen_ids:
                continue
            seen_ids.add(model_id)
            merged.append(item)

    if not merged:
        return JSONResponse(
            content=_error_body("No upstream model endpoints are reachable", "upstream_error"),
            status_code=503,
        )

    return JSONResponse(content={"object": "list", "data": merged})


@router.get("/v1/models/{model_id:path}")
async def openai_model_by_id(model_id: str, request: Request):
    """Resolve a single model id by querying each backend in priority order."""
    if not model_id:
        return JSONResponse(content=_error_body("Model id is required"), status_code=400)

    services = [
        (TG_URL, None),
        (I2T_URL, None),
        (STT_URL, None),
        (TTS_URL, None),
        (IMG_URL, _imagegen_headers(request)),
    ]
    for base_url, headers in services:
        try:
            r = await http_client_default.get(
                f"{base_url}/v1/models/{model_id}",
                headers=headers if headers else None,
                timeout=8.0,
            )
        except Exception:
            continue
        if r.status_code == 200:
            return Response(
                content=r.content,
                status_code=200,
                headers={"content-type": r.headers.get("content-type", "application/json")},
            )

    return JSONResponse(
        content=_error_body(f"Model not found: {model_id}", "invalid_request_error"),
        status_code=404,
    )


@router.post("/v1/responses")
async def openai_responses(request: Request):
    """Proxy OpenAI Responses requests to I2T backend (edge or cloud mode)."""
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse(content=_error_body("Invalid JSON"), status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse(content=_error_body("JSON body must be an object"), status_code=400)
    if "input" not in payload:
        return JSONResponse(content=_error_body("'input' array is required"), status_code=400)
    if "model" not in payload:
        return JSONResponse(content=_error_body("'model' is required"), status_code=400)
    model = payload.get("model")
    if not isinstance(model, str) or not model.strip():
        return JSONResponse(
            content=_error_body("'model' must be a non-empty string"),
            status_code=400,
        )

    # Resolve upstream (edge or cloud)
    base_url, upstream_headers, err = _resolve_i2t_upstream(request)
    if err is not None:
        return err
    
    is_cloud_mode = base_url and _is_openai_base(base_url)
    
    forward_payload = dict(payload)
    payload_sid = forward_payload.pop("session_id", None)
    payload_sid = payload_sid.strip() if isinstance(payload_sid, str) else ""
    header_sid = (request.headers.get("X-Session-Id") or request.headers.get("x-session-id") or "").strip()
    effective_sid = payload_sid or header_sid

    auth = (request.headers.get("Authorization") or "").strip()
    if auth and "Authorization" not in upstream_headers:
        upstream_headers["Authorization"] = auth
    if effective_sid:
        upstream_headers["X-Session-Id"] = effective_sid

    if is_cloud_mode:
        # Cloud mode: use OpenAI's vision API
        response = await _proxy_json_post(
            upstream_url=_make_v1_url(base_url, "responses"),
            payload=forward_payload,
            headers=upstream_headers if upstream_headers else None,
            on_close=None,
        )
        if hasattr(response, "headers") and effective_sid:
            response.headers["X-Session-Id"] = effective_sid
        return response
    
    # Edge mode: use local I2T backend with NPU arbitration
    max_attempts = 2
    last_response: Response | JSONResponse | None = None

    for attempt in range(1, max_attempts + 1):
        try:
            npu_meta, release_npu = await acquire_npu_slot_handle("openai-responses-i2t")
        except NpuBusyError as exc:
            return _npu_busy_response(exc)

        response = await _proxy_json_post(
            upstream_url=f"{I2T_URL}/v1/responses",
            payload=forward_payload,
            headers=upstream_headers if upstream_headers else None,
            on_close=release_npu,
        )

        if hasattr(response, "headers"):
            response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
            if effective_sid:
                response.headers["X-Session-Id"] = effective_sid

        last_response = response
        if attempt >= max_attempts or not _is_retryable_i2t_response(response):
            return response

        # One bounded retry for transient I2T restarts.
        await asyncio.sleep(1.0)

    return last_response or JSONResponse(
        content=_error_body("I2T request failed after retries", "upstream_error"),
        status_code=503,
    )


@router.post("/v1/chat/completions")
async def openai_chat_completions(request: Request):
    """Route text chat completions to TG and reject I2T payloads."""
    try:
        payload = await request.json()
    except Exception:
        return JSONResponse(content=_error_body("Invalid JSON"), status_code=400)
    if not isinstance(payload, dict):
        return JSONResponse(content=_error_body("JSON body must be an object"), status_code=400)

    if is_i2t_payload_wanted(payload):
        return JSONResponse(
            content=_error_body(
                "Image-To-Text is exposed only via POST /v1/responses. "
                "Do not send image/session I2T payloads to /v1/chat/completions.",
                "invalid_request_error",
            ),
            status_code=400,
        )

    model = payload.get("model", TG_ORCHESTRATOR_MODEL_ID)
    if not isinstance(model, str) or not model.strip():
        return JSONResponse(
            content=_error_body("'model' must be a non-empty string"),
            status_code=400,
        )
    model = model.strip()

    forward_payload = dict(payload)
    # If using orchestrator model, fetch active model from backend
    if model == TG_ORCHESTRATOR_MODEL_ID:
        try:
            r = await http_client_default.get(
                f"{TG_URL}/v1/internal/models",
                headers={"X-Internal-API-Key": TG_INTERNAL_API_KEY} if TG_INTERNAL_API_KEY else None,
                timeout=10.0,
            )
            if r.status_code == 200:
                body = r.json()
                active_model_id = body.get("active_model_id", "")
                if active_model_id:
                    forward_payload["model"] = active_model_id
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
    else:
    # Otherwise, pass model through as-is
        pass

    owner = "openai-chat-stream" if bool(forward_payload.get("stream", False)) else "openai-chat"
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle(owner)
    except NpuBusyError as exc:
        return _npu_busy_response(exc)

    response = await _proxy_json_post(
        upstream_url=f"{TG_URL}/v1/chat/completions",
        payload=forward_payload,
        headers={"X-Internal-API-Key": TG_INTERNAL_API_KEY} if TG_INTERNAL_API_KEY else None,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.post("/v1/audio/transcriptions")
async def openai_audio_transcriptions(request: Request):
    """Proxy OpenAI transcription calls to STT backend."""
    return await _proxy_openai_stt_with_alias(
        request,
        upstream_path="/v1/audio/transcriptions",
        npu_owner="openai-stt-transcriptions",
    )


@router.post("/v1/audio/translations")
async def openai_audio_translations(request: Request):
    """Proxy OpenAI translation calls to STT backend."""
    return await _proxy_openai_stt_with_alias(
        request,
        upstream_path="/v1/audio/translations",
        npu_owner="openai-stt-translations",
    )


@router.post("/api/tts/speech")
async def tts_speech_api(request: Request):
    """Legacy helper route for TTS speech generation."""
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle("openai-tts-api")
    except NpuBusyError as exc:
        return _npu_busy_response(exc)
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{TTS_URL}/v1/audio/speech",
        client=http_client_stream,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.post("/v1/audio/speech")
async def openai_audio_speech(request: Request):
    """Proxy OpenAI speech synthesis to TTS backend."""
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle("openai-tts-speech")
    except NpuBusyError as exc:
        return _npu_busy_response(exc)
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{TTS_URL}/v1/audio/speech",
        client=http_client_stream,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.get("/v1/realtime")
async def openai_realtime_info(request: Request):
    """Proxy realtime capability metadata endpoint to STT backend."""
    return await _proxy_raw_request(
        request,
        method="GET",
        upstream_url=f"{STT_URL}/v1/realtime",
        client=http_client_stream,
    )


@router.post("/v1/realtime/sessions")
async def openai_realtime_sessions(request: Request):
    """Proxy realtime session creation to STT backend."""
    return await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}/v1/realtime/sessions",
        client=http_client_stream,
    )


@router.get("/v1/realtime/sessions/{session_id}")
async def openai_realtime_session_status(session_id: str, request: Request):
    """Proxy realtime session status lookup to STT backend."""
    return await _proxy_raw_request(
        request,
        method="GET",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}",
        client=http_client_stream,
    )


@router.delete("/v1/realtime/sessions/{session_id}")
async def openai_realtime_session_delete(session_id: str, request: Request):
    """Proxy realtime session deletion to STT backend."""
    return await _proxy_raw_request(
        request,
        method="DELETE",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}",
        client=http_client_stream,
    )


@router.post("/v1/realtime/sessions/{session_id}/audio")
async def openai_realtime_session_audio(session_id: str, request: Request):
    """Proxy realtime audio append/commit requests to STT backend."""
    return await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}/audio",
        client=http_client_stream,
    )


@router.post("/v1/realtime/sessions/{session_id}/finalize")
async def openai_realtime_session_finalize(session_id: str, request: Request):
    """Proxy realtime finalize requests to STT backend."""
    return await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}/finalize",
        client=http_client_stream,
    )
