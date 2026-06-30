#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Common helpers shared across use-case route modules.
# ---------------------------------------------------------------------

import base64
import binascii
import inspect
import json
import time
from typing import Any, Awaitable, Callable, Optional
from urllib.parse import urlparse

import httpx
from fastapi import HTTPException, Request, WebSocket
from fastapi.responses import JSONResponse, Response, StreamingResponse

from .context import http_client_stream


def _now() -> float:
    """Return high-resolution monotonic timestamp."""
    return time.perf_counter()


def _ms_since(t0: float) -> float:
    """Return elapsed milliseconds since timestamp t0."""
    return round((time.perf_counter() - t0) * 1000.0, 3)


def _error_body(message: str, err_type: str = "invalid_request_error") -> dict:
    """Build standardized OpenAI-style error body."""
    return {"error": {"message": message, "type": err_type, "code": err_type}}


def _proxy_header_subset(headers: httpx.Headers) -> dict:
    """Keep safe response headers while dropping hop-by-hop/internal headers."""
    out: dict = {}
    for key, value in headers.items():
        lk = key.lower()
        if lk in ("content-type", "cache-control", "content-disposition", "retry-after"):
            out[key] = value
        elif lk.startswith("x-") or lk.startswith("openai-"):
            out[key] = value
    return out


def _header_float(headers: httpx.Headers, key: str) -> Optional[float]:
    """Parse numeric timing header value when present."""
    raw = headers.get(key)
    if raw is None:
        return None
    try:
        return round(float(raw), 3)
    except Exception:
        return None


def _merge_numeric_timing(target: dict, key: str, value: Optional[float]) -> None:
    """Set timing key only when target lacks a valid positive value."""
    if not isinstance(target, dict) or value is None:
        return
    current = target.get(key)
    current_num = current if isinstance(current, (int, float)) else None
    if current_num is None or current_num <= 0:
        target[key] = value


def _media_type_from_content_type(content_type: str) -> str:
    """Extract media-type component from full Content-Type header."""
    return content_type.split(";", 1)[0].strip() if content_type else ""


async def _response_from_upstream(upstream: httpx.Response):
    """Convert upstream streamed response into FastAPI Response/StreamingResponse."""
    return await _response_from_upstream_with_close(upstream, on_close=None)


async def _run_on_close(
    on_close: Optional[Callable[[], Optional[Awaitable[None]]]],
) -> None:
    """Best-effort close callback executor that supports sync and async callables."""
    if on_close is None:
        return
    try:
        maybe_awaitable = on_close()
        if inspect.isawaitable(maybe_awaitable):
            await maybe_awaitable
    except Exception:
        # Cleanup callbacks must not break response forwarding.
        return


async def _response_from_upstream_with_close(
    upstream: httpx.Response,
    *,
    on_close: Optional[Callable[[], Optional[Awaitable[None]]]] = None,
):
    """Convert upstream streamed response into FastAPI Response/StreamingResponse."""
    headers = _proxy_header_subset(upstream.headers)
    content_type = ""
    for key in list(headers.keys()):
        if key.lower() == "content-type":
            content_type = headers.pop(key)
            break

    media_type = _media_type_from_content_type(content_type)
    if media_type == "text/event-stream":

        async def stream_proxy():
            try:
                async for chunk in upstream.aiter_bytes():
                    yield chunk
            finally:
                await upstream.aclose()
                await _run_on_close(on_close)

        return StreamingResponse(
            stream_proxy(),
            status_code=upstream.status_code,
            media_type=media_type,
            headers=headers,
        )

    body = await upstream.aread()
    await upstream.aclose()
    await _run_on_close(on_close)
    if content_type:
        headers["content-type"] = content_type
    return Response(
        content=body,
        status_code=upstream.status_code,
        headers=headers,
    )


async def _proxy_raw_request(
    request: Request,
    *,
    method: str,
    upstream_url: str,
    client: httpx.AsyncClient,
    extra_headers: Optional[dict] = None,
    on_close: Optional[Callable[[], Optional[Awaitable[None]]]] = None,
):
    """Proxy request body/query/selected headers to upstream endpoint."""
    raw_body = await request.body()
    headers = {}
    content_type = request.headers.get("content-type")
    if content_type:
        headers["Content-Type"] = content_type
    auth = request.headers.get("authorization")
    if auth:
        headers["Authorization"] = auth
    if extra_headers:
        headers.update(extra_headers)

    try:
        req = client.build_request(
            method.upper(),
            upstream_url,
            content=raw_body,
            headers=headers,
            params=request.query_params,
        )
        upstream = await client.send(req, stream=True)
    except httpx.HTTPError as exc:
        await _run_on_close(on_close)
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )

    return await _response_from_upstream_with_close(upstream, on_close=on_close)


async def _proxy_json_post(
    *,
    upstream_url: str,
    payload: dict,
    headers: Optional[dict] = None,
    client: httpx.AsyncClient = http_client_stream,
    on_close: Optional[Callable[[], Optional[Awaitable[None]]]] = None,
):
    """Proxy JSON POST payload to upstream endpoint and stream response through."""
    req_headers = {"Content-Type": "application/json"}
    if headers:
        req_headers.update(headers)
    try:
        req = client.build_request(
            "POST",
            upstream_url,
            json=payload,
            headers=req_headers,
        )
        upstream = await client.send(req, stream=True)
    except httpx.HTTPError as exc:
        await _run_on_close(on_close)
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )
    return await _response_from_upstream_with_close(upstream, on_close=on_close)


def _to_int(value: Any, default: int, minimum: Optional[int] = None, maximum: Optional[int] = None) -> int:
    """Parse integer with default and optional clamping bounds."""
    try:
        parsed = int(value)
    except Exception:
        parsed = default
    if minimum is not None and parsed < minimum:
        parsed = minimum
    if maximum is not None and parsed > maximum:
        parsed = maximum
    return parsed


def _to_bool(value: Any, default: bool = False) -> bool:
    """Parse common truthy/falsey string and numeric variants."""
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    if isinstance(value, (int, float)):
        return value != 0
    text = str(value).strip().lower()
    if text in ("1", "true", "yes", "y", "on"):
        return True
    if text in ("0", "false", "no", "n", "off"):
        return False
    return default


def _ws_error_event(message: str, err_type: str = "invalid_request_error", code: Optional[str] = None) -> dict:
    """Build websocket error event payload."""
    code_value = code or err_type
    return {
        "type": "error",
        "error": {
            "message": message,
            "type": err_type,
            "code": code_value,
        },
    }


async def _ws_send_json_safe(websocket: WebSocket, payload: dict) -> bool:
    """Best-effort websocket JSON sender that returns success boolean."""
    try:
        await websocket.send_text(json.dumps(payload))
        return True
    except Exception:
        return False


async def _ws_send_error(
    websocket: WebSocket,
    message: str,
    err_type: str = "invalid_request_error",
    code: Optional[str] = None,
) -> bool:
    """Send standardized websocket error event."""
    return await _ws_send_json_safe(websocket, _ws_error_event(message, err_type=err_type, code=code))


def _parse_upstream_json_response(upstream: httpx.Response) -> dict:
    """Parse upstream JSON body and normalize non-JSON failures."""
    try:
        body = upstream.json()
        return body if isinstance(body, dict) else {"data": body}
    except Exception:
        return {
            "error": {
                "message": (upstream.text or "")[:400] or "Upstream non-JSON response",
                "type": "upstream_error",
                "code": "upstream_error",
            }
        }


def _decode_audio_base64(audio_b64: Any) -> bytes:
    """Decode base64 audio payload with optional data-URL prefix handling."""
    if not isinstance(audio_b64, str) or not audio_b64.strip():
        return b""
    text = audio_b64.strip()
    if "," in text and text.split(",", 1)[0].lower().startswith("data:"):
        text = text.split(",", 1)[1]
    padding = (-len(text)) % 4
    if padding:
        text = text + ("=" * padding)
    return base64.b64decode(text, validate=False)


def _normalize_base_url(raw_url: str, fallback: str) -> str:
    """Normalize endpoint base URL with scheme and no trailing slash."""
    base = (raw_url or fallback or "").strip()
    if not base:
        return fallback
    if not (base.startswith("http://") or base.startswith("https://")):
        base = "https://" + base
    return base.rstrip("/")


def _make_v1_url(base_url: str, suffix: str) -> str:
    """Build full /v1 path while handling bases that already end in /v1."""
    base = base_url.rstrip("/")
    tail = suffix.lstrip("/")
    if base.lower().endswith("/v1"):
        return f"{base}/{tail}"
    return f"{base}/v1/{tail}"


def _is_openai_base(base_url: str) -> bool:
    """Detect whether URL points to an OpenAI-hosted endpoint."""
    try:
        parsed = urlparse(base_url)
        host = (parsed.netloc or parsed.path or "").lower()
        return "openai.com" in host
    except Exception:
        return False


async def _download_image_bytes(image_url: str, image_client: httpx.AsyncClient) -> bytes:
    """Read image bytes from data URL, HTTP(S), or local file path."""
    if image_url.startswith("data:"):
        if "," not in image_url:
            raise HTTPException(status_code=400, detail="Invalid data URL for image_url")
        meta, encoded = image_url.split(",", 1)
        if ";base64" not in meta:
            raise HTTPException(status_code=400, detail="Only base64 data URLs are supported for image_url")
        try:
            return base64.b64decode(encoded, validate=False)
        except (binascii.Error, ValueError) as exc:
            raise HTTPException(status_code=400, detail=f"Invalid base64 data URL: {exc}") from exc

    if image_url.startswith("http://") or image_url.startswith("https://"):
        try:
            r = await image_client.get(image_url, timeout=60.0, follow_redirects=True)
        except httpx.HTTPError as exc:
            raise HTTPException(status_code=400, detail=f"Failed to download image_url: {exc}") from exc
        if r.status_code >= 400:
            raise HTTPException(
                status_code=400,
                detail=f"image_url download failed with status {r.status_code}",
            )
        if not r.content:
            raise HTTPException(status_code=400, detail="Downloaded image_url is empty")
        return r.content

    source = image_url[7:] if image_url.startswith("file://") else image_url
    from pathlib import Path

    p = Path(source)
    if not p.exists() or not p.is_file():
        raise HTTPException(status_code=400, detail=f"image_url file not found: {source}")
    try:
        return p.read_bytes()
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Failed to read image_url file: {exc}") from exc
