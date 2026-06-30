#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Speech-To-Text routes (HTTP + realtime websocket bridge).
# ---------------------------------------------------------------------

import json
import time
from typing import Optional

import httpx
from fastapi import APIRouter, File, Form, Request, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from .common import (
    _decode_audio_base64,
    _error_body,
    _header_float,
    _merge_numeric_timing,
    _ms_since,
    _now,
    _parse_upstream_json_response,
    _proxy_raw_request,
    _to_bool,
    _to_int,
    _ws_send_error,
    _ws_send_json_safe,
)
from .context import STT_URL, http_client_default, http_client_stream, stt_client
from .npu_arbiter import NpuBusyError, acquire_npu_slot, acquire_npu_slot_handle

router = APIRouter()


async def _realtime_create_session_via_http(payload: dict) -> tuple[int, dict]:
    """Create STT realtime session via HTTP and normalize error output."""
    try:
        async with acquire_npu_slot("stt-realtime-create"):
            upstream = await http_client_stream.post(f"{STT_URL}/v1/realtime/sessions", json=payload)
    except NpuBusyError as exc:
        return 503, _error_body(str(exc), "npu_busy")
    except httpx.HTTPError as exc:
        return 503, _error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error")
    return upstream.status_code, _parse_upstream_json_response(upstream)


async def _realtime_audio_append_via_http(
    session_id: str,
    audio_bytes: bytes,
    *,
    sample_rate_hz: int,
    commit: bool = False,
    language: str = "",
    task: str = "",
) -> tuple[int, dict]:
    """Append audio bytes to STT realtime session with optional commit/task params."""
    params = {
        "sample_rate_hz": str(sample_rate_hz),
        "commit": "1" if commit else "0",
    }
    if language:
        params["language"] = language
    if task:
        params["task"] = task
    try:
        async with acquire_npu_slot("stt-realtime-audio"):
            upstream = await http_client_stream.post(
                f"{STT_URL}/v1/realtime/sessions/{session_id}/audio",
                params=params,
                content=audio_bytes,
                headers={"Content-Type": "application/octet-stream"},
            )
    except NpuBusyError as exc:
        return 503, _error_body(str(exc), "npu_busy")
    except httpx.HTTPError as exc:
        return 503, _error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error")
    return upstream.status_code, _parse_upstream_json_response(upstream)


async def _realtime_finalize_via_http(session_id: str) -> tuple[int, dict]:
    """Finalize STT realtime session and request final transcript."""
    try:
        async with acquire_npu_slot("stt-realtime-finalize"):
            upstream = await http_client_stream.post(
                f"{STT_URL}/v1/realtime/sessions/{session_id}/finalize",
            )
    except NpuBusyError as exc:
        return 503, _error_body(str(exc), "npu_busy")
    except httpx.HTTPError as exc:
        return 503, _error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error")
    return upstream.status_code, _parse_upstream_json_response(upstream)


async def _realtime_delete_session_via_http(session_id: str) -> None:
    """Best-effort deletion of STT realtime session."""
    try:
        await http_client_default.delete(f"{STT_URL}/v1/realtime/sessions/{session_id}")
    except Exception:
        return


@router.get("/api/stt/models")
async def stt_models():
    """List STT models using OpenAI client compatibility layer."""
    models = await stt_client.models.list()
    return JSONResponse(content=models.model_dump())


@router.get("/api/stt/languages")
async def stt_languages():
    """List supported STT languages from backend service."""
    r = await http_client_default.get(f"{STT_URL}/languages")
    return JSONResponse(content=r.json(), status_code=r.status_code)


@router.post("/api/stt/transcribe")
async def stt_transcribe(
    file: UploadFile = File(...),
    language: Optional[str] = Form(None),
    response_format: Optional[str] = Form("verbose_json"),
    task: Optional[str] = Form("transcribe"),
):
    """Handle multipart STT transcribe/translate call and enrich timing metadata."""
    t0 = _now()
    audio_bytes = await file.read()
    read_file_ms = _ms_since(t0)
    filename = file.filename or "audio.wav"
    content_type = file.content_type or "audio/wav"
    fmt = response_format or "verbose_json"
    task_norm = (task or "transcribe").strip().lower()
    if task_norm not in ("transcribe", "translate"):
        return JSONResponse(
            content=_error_body("Invalid 'task'. Must be 'transcribe' or 'translate'"),
            status_code=400,
        )

    endpoint = "/v1/audio/translations" if task_norm == "translate" else "/v1/audio/transcriptions"
    form_data = {"model": "whisper-tiny", "response_format": fmt}
    if language:
        form_data["language"] = language

    npu_wait_ms = 0.0
    try:
        t_up = _now()
        async with acquire_npu_slot("stt-transcribe") as npu_meta:
            npu_wait_ms = float(npu_meta.get("wait_ms", 0.0))
            upstream = await http_client_stream.post(
                f"{STT_URL}{endpoint}",
                files={"file": (filename, audio_bytes, content_type)},
                data=form_data,
            )
        upstream_ms = _ms_since(t_up)
    except NpuBusyError as exc:
        response = JSONResponse(content=_error_body(str(exc), "npu_busy"), status_code=503)
        response.headers["Retry-After"] = "2"
        return response
    except httpx.HTTPError as exc:
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )

    upstream_headers = upstream.headers
    upstream_header_timing = {
        "request_total_ms": _header_float(upstream_headers, "X-Request-Total-Ms"),
        "save_file_ms": _header_float(upstream_headers, "X-Save-File-Ms"),
        "lock_wait_ms": _header_float(upstream_headers, "X-Lock-Wait-Ms"),
        "engine_total_ms": _header_float(upstream_headers, "X-Engine-Total-Ms"),
        "engine_config_ms": _header_float(upstream_headers, "X-Engine-Config-Ms"),
        "engine_start_ms": _header_float(upstream_headers, "X-Engine-Start-Ms"),
        "engine_feed_ms": _header_float(upstream_headers, "X-Engine-Feed-Ms"),
        "engine_wait_ms": _header_float(upstream_headers, "X-Engine-Wait-Ms"),
        "engine_stop_ms": _header_float(upstream_headers, "X-Engine-Stop-Ms"),
    }
    try:
        body = upstream.json()
        if isinstance(body, dict):
            nested_raw = body.get("text")
            if isinstance(nested_raw, str):
                nested_str = nested_raw.strip()
                if nested_str.startswith("{") and nested_str.endswith("}"):
                    try:
                        nested_body = json.loads(nested_str)
                        if isinstance(nested_body, dict) and (
                            "text" in nested_body or "duration" in nested_body or "segments" in nested_body
                        ):
                            if "x_timing" not in nested_body and isinstance(body.get("x_timing"), dict):
                                nested_body["x_timing"] = body["x_timing"]
                            body = nested_body
                    except Exception:
                        pass

            upstream_timing = body.get("x_timing", {})
            if not isinstance(upstream_timing, dict):
                upstream_timing = {}
            for t_key, t_val in upstream_header_timing.items():
                _merge_numeric_timing(upstream_timing, t_key, t_val)
            if upstream_timing:
                body["x_timing"] = upstream_timing
            body["x_orchestrator_timing"] = {
                "read_file_ms": read_file_ms,
                "npu_wait_ms": npu_wait_ms,
                "upstream_ms": upstream_ms,
                "request_total_ms": _ms_since(t0),
            }
        response = JSONResponse(content=body, status_code=upstream.status_code)
        response.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
        for t_key, t_val in upstream_header_timing.items():
            if t_val is not None:
                response.headers[f"X-Upstream-{t_key.replace('_', '-').title()}"] = str(t_val)
        return response
    except Exception:
        fallback = {
            "text": upstream.text,
            "x_timing": {key: val for key, val in upstream_header_timing.items() if val is not None},
            "x_orchestrator_timing": {
                "read_file_ms": read_file_ms,
                "npu_wait_ms": npu_wait_ms,
                "upstream_ms": upstream_ms,
                "request_total_ms": _ms_since(t0),
            },
        }
        response = JSONResponse(content=fallback, status_code=upstream.status_code)
        response.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
        for t_key, t_val in upstream_header_timing.items():
            if t_val is not None:
                response.headers[f"X-Upstream-{t_key.replace('_', '-').title()}"] = str(t_val)
        return response


@router.post("/api/stt/realtime/sessions")
async def stt_realtime_create(request: Request):
    """Create realtime session through /api namespace wrapper."""
    t0 = _now()
    try:
        raw = await request.body()
        payload = json.loads(raw.decode("utf-8")) if raw else {}
    except Exception:
        return JSONResponse(content=_error_body("Invalid JSON"), status_code=400)
    if payload is None:
        payload = {}
    if not isinstance(payload, dict):
        return JSONResponse(content=_error_body("JSON body must be an object"), status_code=400)

    npu_wait_ms = 0.0
    try:
        t_up = _now()
        async with acquire_npu_slot("stt-realtime-create-http") as npu_meta:
            npu_wait_ms = float(npu_meta.get("wait_ms", 0.0))
            upstream = await http_client_stream.post(f"{STT_URL}/v1/realtime/sessions", json=payload)
        upstream_ms = _ms_since(t_up)
    except NpuBusyError as exc:
        response = JSONResponse(content=_error_body(str(exc), "npu_busy"), status_code=503)
        response.headers["Retry-After"] = "2"
        return response
    except httpx.HTTPError as exc:
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )

    try:
        body = upstream.json()
    except Exception:
        body = {"error": {"message": upstream.text[:400], "type": "upstream_error", "code": "upstream_error"}}

    if isinstance(body, dict):
        body["x_orchestrator_timing"] = {
            "parse_ms": _ms_since(t0),
            "npu_wait_ms": npu_wait_ms,
            "upstream_ms": upstream_ms,
            "request_total_ms": _ms_since(t0),
        }
    response = JSONResponse(content=body, status_code=upstream.status_code)
    response.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
    return response


@router.get("/api/stt/realtime/sessions/{session_id}")
async def stt_realtime_status(session_id: str):
    """Get realtime session state from STT backend."""
    try:
        upstream = await http_client_default.get(f"{STT_URL}/v1/realtime/sessions/{session_id}")
    except httpx.HTTPError as exc:
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )
    try:
        body = upstream.json()
    except Exception:
        body = {"error": {"message": upstream.text[:400], "type": "upstream_error", "code": "upstream_error"}}
    return JSONResponse(content=body, status_code=upstream.status_code)


@router.delete("/api/stt/realtime/sessions/{session_id}")
async def stt_realtime_delete(session_id: str):
    """Delete realtime session in STT backend."""
    try:
        upstream = await http_client_default.delete(f"{STT_URL}/v1/realtime/sessions/{session_id}")
    except httpx.HTTPError as exc:
        return JSONResponse(
            content=_error_body(f"Upstream request failed: {str(exc)[:300]}", "upstream_error"),
            status_code=503,
        )
    try:
        body = upstream.json()
    except Exception:
        body = {"error": {"message": upstream.text[:400], "type": "upstream_error", "code": "upstream_error"}}
    return JSONResponse(content=body, status_code=upstream.status_code)


@router.post("/api/stt/realtime/sessions/{session_id}/audio")
async def stt_realtime_audio(session_id: str, request: Request):
    """Pass-through realtime audio append endpoint."""
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle("stt-realtime-audio-http")
    except NpuBusyError as exc:
        response = JSONResponse(content=_error_body(str(exc), "npu_busy"), status_code=503)
        response.headers["Retry-After"] = "2"
        return response
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}/audio",
        client=http_client_stream,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.post("/api/stt/realtime/sessions/{session_id}/finalize")
async def stt_realtime_finalize(session_id: str, request: Request):
    """Pass-through realtime finalize endpoint."""
    try:
        npu_meta, release_npu = await acquire_npu_slot_handle("stt-realtime-finalize-http")
    except NpuBusyError as exc:
        response = JSONResponse(content=_error_body(str(exc), "npu_busy"), status_code=503)
        response.headers["Retry-After"] = "2"
        return response
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{STT_URL}/v1/realtime/sessions/{session_id}/finalize",
        client=http_client_stream,
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.websocket("/ws/stt/realtime")
@router.websocket("/v1/realtime")
async def stt_realtime_websocket(websocket: WebSocket):
    """WebSocket bridge that translates client events to STT realtime HTTP API calls."""
    await websocket.accept()

    session_id: Optional[str] = None
    model = (websocket.query_params.get("model") or "gpt-4o-mini-transcribe").strip() or "gpt-4o-mini-transcribe"
    language = (websocket.query_params.get("language") or "en").strip() or "en"
    task = (websocket.query_params.get("task") or "transcribe").strip().lower() or "transcribe"
    if task not in ("transcribe", "translate"):
        task = "transcribe"
    sample_rate_hz = _to_int(websocket.query_params.get("sample_rate_hz"), 16000, 8000, 48000)
    max_duration_s = _to_int(websocket.query_params.get("max_duration_s"), 30, 5, 120)
    vad_threshold = websocket.query_params.get("vad_threshold")
    vad_hangover_ms = websocket.query_params.get("vad_hangover_ms")
    auto_create = _to_bool(websocket.query_params.get("autocreate"), False)
    auto_cleanup = not _to_bool(websocket.query_params.get("keep_session"), False)

    async def _emit_upstream_error(prefix: str, payload: dict, status_code: int = 400) -> None:
        if isinstance(payload, dict) and isinstance(payload.get("error"), dict):
            err = payload.get("error", {})
            message = err.get("message") or prefix
            err_type = err.get("type") or "upstream_error"
            err_code = err.get("code") or "upstream_error"
            await _ws_send_error(websocket, f"{prefix}: {message}", err_type=err_type, code=str(err_code))
            return
        await _ws_send_error(websocket, f"{prefix} (status {status_code})", err_type="upstream_error")

    async def _create_session(session_overrides: Optional[dict] = None) -> bool:
        nonlocal session_id, model, language, task, sample_rate_hz, max_duration_s

        payload: dict = {
            "model": model,
            "language": language,
            "task": task,
            "sample_rate_hz": sample_rate_hz,
            "max_duration_s": max_duration_s,
            "turn_detection": {
                "type": "server_vad",
            },
        }
        if vad_threshold not in (None, ""):
            try:
                payload["turn_detection"]["threshold"] = float(vad_threshold)
            except Exception:
                pass
        if vad_hangover_ms not in (None, ""):
            try:
                payload["turn_detection"]["silence_duration_ms"] = int(vad_hangover_ms)
            except Exception:
                pass

        if isinstance(session_overrides, dict):
            override_model = session_overrides.get("model")
            override_language = session_overrides.get("language")
            override_task = session_overrides.get("task")
            override_sr = session_overrides.get("sample_rate_hz")
            override_max_s = session_overrides.get("max_duration_s")
            payload.update(
                {
                    "model": str(override_model).strip() if override_model else payload["model"],
                    "language": str(override_language).strip() if override_language else payload["language"],
                    "task": str(override_task).strip().lower() if override_task else payload["task"],
                    "sample_rate_hz": _to_int(override_sr, payload["sample_rate_hz"], 8000, 48000),
                    "max_duration_s": _to_int(override_max_s, payload["max_duration_s"], 5, 120),
                }
            )
            td = session_overrides.get("turn_detection")
            if isinstance(td, dict):
                payload["turn_detection"] = dict(payload.get("turn_detection", {}))
                if "threshold" in td:
                    try:
                        payload["turn_detection"]["threshold"] = float(td.get("threshold"))
                    except Exception:
                        pass
                if "silence_duration_ms" in td:
                    try:
                        payload["turn_detection"]["silence_duration_ms"] = int(td.get("silence_duration_ms"))
                    except Exception:
                        pass

        if payload.get("task") not in ("transcribe", "translate"):
            payload["task"] = "transcribe"

        status_code, body = await _realtime_create_session_via_http(payload)
        if status_code >= 400:
            await _emit_upstream_error("Realtime session create failed", body, status_code)
            return False

        new_session_id = str(body.get("id", "")).strip() if isinstance(body, dict) else ""
        if not new_session_id:
            await _ws_send_error(
                websocket,
                "Realtime session create returned no session id",
                err_type="upstream_error",
                code="upstream_error",
            )
            return False

        session_id = new_session_id
        model = str(body.get("model", payload["model"])).strip() or payload["model"]
        language = str(body.get("language", payload["language"])).strip() or payload["language"]
        task = str(body.get("task", payload["task"])).strip().lower() or payload["task"]
        sample_rate_hz = _to_int(body.get("sample_rate_hz"), payload["sample_rate_hz"], 8000, 48000)
        max_duration_s = _to_int(body.get("max_duration_s"), payload["max_duration_s"], 5, 120)

        await _ws_send_json_safe(
            websocket,
            {
                "type": "session.created",
                "session": body,
            },
        )
        return True

    async def _finalize_session_and_emit() -> bool:
        nonlocal session_id
        if not session_id:
            await _ws_send_error(websocket, "No active realtime session", code="session_missing")
            return False
        status_code, body = await _realtime_finalize_via_http(session_id)
        if status_code >= 400:
            await _emit_upstream_error("Realtime finalize failed", body, status_code)
            return False

        await _ws_send_json_safe(
            websocket,
            {
                "type": "conversation.item.input_audio_transcription.completed",
                "session_id": session_id,
                "text": body.get("text", ""),
                "segments": body.get("segments", []),
                "audio_ms_total": body.get("audio_ms_total", 0),
                "audio_ms_pending": body.get("audio_ms_pending", 0),
                "x_timing": body.get("x_timing", {}),
            },
        )
        await _ws_send_json_safe(
            websocket,
            {
                "type": "response.completed",
                "session_id": session_id,
                "response": {
                    "text": body.get("text", ""),
                    "segments": body.get("segments", []),
                },
                "audio_ms_total": body.get("audio_ms_total", 0),
                "x_timing": body.get("x_timing", {}),
            },
        )
        return True

    await _ws_send_json_safe(
        websocket,
        {
            "type": "session.ready",
            "object": "realtime.ws",
            "transport": "websocket",
            "endpoints": {
                "ws": "/ws/stt/realtime",
                "openai_ws": "/v1/realtime",
                "http_session_create": "/v1/realtime/sessions",
            },
            "supported_events": [
                "session.create",
                "session.update",
                "input_audio_buffer.append",
                "input_audio_buffer.commit",
                "response.create",
                "session.close",
                "ping",
            ],
        },
    )

    if auto_create:
        await _create_session()

    try:
        while True:
            frame = await websocket.receive()
            frame_type = frame.get("type")
            if frame_type == "websocket.disconnect":
                break

            payload: dict = {}
            audio_bytes: bytes = b""

            if frame.get("bytes") is not None:
                audio_bytes = bytes(frame.get("bytes") or b"")
                payload = {"type": "input_audio_buffer.append"}
            else:
                text_payload = frame.get("text") or ""
                if not text_payload:
                    continue
                try:
                    payload = json.loads(text_payload)
                except Exception:
                    await _ws_send_error(websocket, "Invalid JSON event")
                    continue
                if not isinstance(payload, dict):
                    await _ws_send_error(websocket, "Event payload must be a JSON object")
                    continue

            evt = str(payload.get("type", "")).strip().lower()
            if not evt:
                if "audio" in payload or "audio_base64" in payload:
                    evt = "input_audio_buffer.append"
                else:
                    await _ws_send_error(websocket, "Missing event 'type'")
                    continue

            if evt == "ping":
                await _ws_send_json_safe(websocket, {"type": "pong", "ts": round(time.time(), 3)})
                continue

            if evt == "session.create":
                overrides = payload.get("session")
                if not isinstance(overrides, dict):
                    overrides = payload if isinstance(payload, dict) else {}
                await _create_session(overrides)
                continue

            if evt == "session.update":
                session = payload.get("session")
                if isinstance(session, dict):
                    if session.get("model"):
                        model = str(session.get("model")).strip() or model
                    if session.get("language"):
                        language = str(session.get("language")).strip() or language
                    if session.get("task"):
                        new_task = str(session.get("task")).strip().lower()
                        if new_task in ("transcribe", "translate"):
                            task = new_task
                    sample_rate_hz = _to_int(session.get("sample_rate_hz"), sample_rate_hz, 8000, 48000)
                    max_duration_s = _to_int(session.get("max_duration_s"), max_duration_s, 5, 120)
                await _ws_send_json_safe(
                    websocket,
                    {
                        "type": "session.updated",
                        "session_id": session_id,
                        "model": model,
                        "language": language,
                        "task": task,
                        "sample_rate_hz": sample_rate_hz,
                        "max_duration_s": max_duration_s,
                    },
                )
                continue

            if evt in ("input_audio_buffer.commit", "response.create"):
                if not session_id and not await _create_session():
                    continue
                await _finalize_session_and_emit()
                continue

            if evt in ("session.close", "close"):
                if session_id:
                    await _realtime_delete_session_via_http(session_id)
                    await _ws_send_json_safe(
                        websocket,
                        {
                            "type": "session.closed",
                            "session_id": session_id,
                        },
                    )
                    session_id = None
                await websocket.close(code=1000)
                return

            if evt not in ("input_audio_buffer.append", "append_audio", "audio.append"):
                await _ws_send_error(websocket, f"Unsupported realtime event '{evt}'")
                continue

            if not session_id and not await _create_session():
                continue

            if not audio_bytes:
                audio_b64 = payload.get("audio", payload.get("audio_base64"))
                try:
                    audio_bytes = _decode_audio_base64(audio_b64)
                except Exception:
                    await _ws_send_error(websocket, "Invalid base64 audio payload")
                    continue

            if not audio_bytes:
                await _ws_send_error(websocket, "Empty audio payload")
                continue

            request_sample_rate_hz = _to_int(payload.get("sample_rate_hz"), sample_rate_hz, 8000, 48000)
            request_language = str(payload.get("language", language)).strip() or language
            request_task = str(payload.get("task", task)).strip().lower() or task
            if request_task not in ("transcribe", "translate"):
                request_task = task
            commit = _to_bool(payload.get("commit"), False)

            status_code, body = await _realtime_audio_append_via_http(
                session_id,
                audio_bytes,
                sample_rate_hz=request_sample_rate_hz,
                commit=commit,
                language=request_language,
                task=request_task,
            )
            if status_code >= 400:
                await _emit_upstream_error("Realtime audio append failed", body, status_code)
                continue

            await _ws_send_json_safe(
                websocket,
                {
                    "type": "input_audio_buffer.appended",
                    "session_id": session_id,
                    "speech_detected": body.get("speech_detected", False),
                    "segment_committed": body.get("segment_committed", False),
                    "delta_text": body.get("delta_text", ""),
                    "text": body.get("text", ""),
                    "audio_ms_total": body.get("audio_ms_total", 0),
                    "audio_ms_pending": body.get("audio_ms_pending", 0),
                    "chunks_received": body.get("chunks_received", 0),
                    "sample_rate_hz": body.get("sample_rate_hz", request_sample_rate_hz),
                    "x_timing": body.get("x_timing", {}),
                },
            )

            delta_text = str(body.get("delta_text", "")).strip()
            if delta_text:
                await _ws_send_json_safe(
                    websocket,
                    {
                        "type": "conversation.item.input_audio_transcription.delta",
                        "session_id": session_id,
                        "delta": delta_text,
                        "text": body.get("text", ""),
                    },
                )
            if body.get("segment_committed"):
                await _ws_send_json_safe(
                    websocket,
                    {
                        "type": "conversation.item.input_audio_transcription.completed",
                        "session_id": session_id,
                        "text": body.get("text", ""),
                        "audio_ms_total": body.get("audio_ms_total", 0),
                        "audio_ms_pending": body.get("audio_ms_pending", 0),
                        "x_timing": body.get("x_timing", {}),
                    },
                )
    except WebSocketDisconnect:
        pass
    except Exception as exc:
        await _ws_send_error(
            websocket,
            f"Realtime websocket internal error: {str(exc)[:300]}",
            err_type="server_error",
            code="server_error",
        )
        try:
            await websocket.close(code=1011)
        except Exception:
            pass
    finally:
        if session_id and auto_cleanup:
            await _realtime_delete_session_via_http(session_id)
