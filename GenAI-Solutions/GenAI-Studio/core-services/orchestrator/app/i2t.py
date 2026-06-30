#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Image-To-Text routing helpers.
# ---------------------------------------------------------------------

import asyncio
import os
from typing import Any, Optional

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from .common import (
    _error_body,
    _is_openai_base,
    _make_v1_url,
    _normalize_base_url,
    _proxy_json_post,
)
from .context import (
    I2T_URL,
    OPENAI_API_KEY,
    OPENAI_BASE_URL,
    http_client_default,
    http_client_stream,
    i2t_client,
)

router = APIRouter()

_DEFAULT_I2T_SESSION_ID = "__default__"


def _to_session_id(value: Any) -> str:
    if isinstance(value, str):
        sid = value.strip()
        if sid:
            return sid
    return ""


def _extract_image_urls_from_messages(messages: list[Any]) -> list[str]:
    urls: list[str] = []
    for message in messages:
        if not isinstance(message, dict):
            continue
        content = message.get("content")
        if isinstance(content, str):
            continue
        if not isinstance(content, list):
            continue
        for part in content:
            if not isinstance(part, dict):
                continue
            ptype = str(part.get("type", "")).lower()
            if ptype not in ("image_url", "input_image", "image"):
                continue
            image_url = part.get("image_url")
            if isinstance(image_url, str) and image_url.strip():
                urls.append(image_url.strip())
            elif isinstance(image_url, dict):
                url = image_url.get("url")
                if isinstance(url, str) and url.strip():
                    urls.append(url.strip())
            elif isinstance(part.get("url"), str) and str(part.get("url")).strip():
                urls.append(str(part.get("url")).strip())
    return urls


async def wait_for_i2t_ready(timeout_sec: float = 30.0) -> bool:
    """Poll I2T health endpoint until ready or timeout."""
    deadline = asyncio.get_event_loop().time() + timeout_sec
    while asyncio.get_event_loop().time() < deadline:
        try:
            r = await http_client_default.get(f"{I2T_URL}/health", timeout=3.0)
            if r.status_code == 200:
                return True
        except Exception:
            pass
        await asyncio.sleep(1.5)
    return False


def is_i2t_payload_wanted(payload: dict) -> bool:
    """Detect I2T payloads misrouted to /v1/chat/completions."""
    if not isinstance(payload, dict):
        return False

    if payload.get("session_id"):
        return True

    if isinstance(payload.get("pixel_values_path"), str) and payload.get("pixel_values_path", "").strip():
        return True

    messages = payload.get("messages")
    if isinstance(messages, list) and _extract_image_urls_from_messages(messages):
        return True

    return False


@router.post("/api/i2t/reset")
async def i2t_reset(request: Request):
    """Reset I2T backend session/KV cache (edge mode only)."""
    try:
        body = await request.json()
    except Exception:
        body = {}

    if not isinstance(body, dict):
        body = {}

    base_url, headers, err = _resolve_i2t_upstream(request)
    if err is not None:
        return err
    if base_url and _is_openai_base(base_url):
        return JSONResponse(
            content=_error_body("Session reset is available only for On-Device mode.", "invalid_request_error"),
            status_code=400,
        )

    payload_sid = _to_session_id(body.get("session_id"))
    header_sid = _to_session_id(request.headers.get("X-Session-Id") or request.headers.get("x-session-id"))
    session_id = payload_sid or header_sid

    reset_headers = {"Content-Type": "application/json"}
    if session_id:
        reset_headers["X-Session-Id"] = session_id

    try:
        r = await http_client_default.post(f"{I2T_URL}/v1/session/reset", headers=reset_headers, timeout=30.0)
    except Exception as exc:
        return JSONResponse(
            content={"error": {"code": "upstream_unreachable", "message": str(exc)[:300]}},
            status_code=503,
        )

    try:
        content = r.json()
    except Exception:
        content = {"error": {"code": "upstream_error", "message": (r.text or "")[:400]}}

    response = JSONResponse(content=content, status_code=r.status_code)
    if session_id:
        response.headers["X-Session-Id"] = session_id
    return response


def _resolve_i2t_upstream(request: Request) -> tuple[Optional[str], dict, Optional[JSONResponse]]:
    """Resolve Image-To-Text upstream base URL and auth headers for edge/cloud modes."""
    mode_raw = (
        request.headers.get("x-i2t-mode")
        or request.headers.get("x-endpoint-mode")
        or os.getenv("I2T_PROVIDER", "edge")
    )
    mode = str(mode_raw).strip().lower()
    base_hdr = request.headers.get("x-i2t-base-url") or request.headers.get("x-endpoint-url") or ""
    incoming_auth = request.headers.get("authorization", "").strip()
    api_key = request.headers.get("x-api-key", "").strip()

    provider = "openai" if mode in ("cloud", "openai") else "edge"
    candidate_base = _normalize_base_url(base_hdr, OPENAI_BASE_URL if provider == "openai" else I2T_URL)
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
    return candidate_base, headers, None


@router.get("/api/i2t/models")
async def i2t_models(request: Request):
    """List I2T models from backend service (edge or cloud)."""
    base_url, headers, err = _resolve_i2t_upstream(request)
    if err is not None:
        return err

    if base_url and _is_openai_base(base_url):
        # For OpenAI, use the standard /v1/models endpoint
        try:
            r = await http_client_default.get(
                _make_v1_url(base_url, "models"),
                headers=headers,
                timeout=10.0,
            )
            if r.status_code == 200:
                return JSONResponse(content=r.json(), status_code=200)
            else:
                return JSONResponse(
                    content=_error_body(f"Failed to fetch models: {r.status_code}", "upstream_error"),
                    status_code=r.status_code,
                )
        except Exception as exc:
            return JSONResponse(
                content=_error_body(f"Failed to fetch models: {str(exc)[:300]}", "upstream_error"),
                status_code=503,
            )
    else:
        # For edge mode, use local client
        models = await i2t_client.models.list()
        return JSONResponse(content=models.model_dump())
