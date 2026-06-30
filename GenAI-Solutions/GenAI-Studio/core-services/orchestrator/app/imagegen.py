#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Image-Generation routes and helper logic.
# ---------------------------------------------------------------------

import asyncio
import json
import os
from typing import Optional
from urllib.parse import urlparse

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse, Response

from .common import (
    _error_body,
    _header_float,
    _merge_numeric_timing,
    _ms_since,
    _now,
    _proxy_raw_request,
)
from .context import (
    I2T_CONTAINER_NAME,
    IMG_URL,
    http_client_default,
    http_client_img,
    http_client_stream,
    img_client,
)
from .i2t import wait_for_i2t_ready
from .npu_arbiter import NpuBusyError, acquire_npu_slot

router = APIRouter()

# Disabling this by default avoids a failure mode where orchestrator stops
# Image2Text and cannot restart it (runtime mount/device constraints), leaving
# I2T inference unavailable. Enable only when host/container runtime supports it.
IMG_I2T_ARBITRATION_ENABLED = os.getenv("IMG_I2T_ARBITRATION_ENABLED", "0").lower() in (
    "1",
    "true",
    "yes",
)


def _imagegen_headers(request: Optional[Request] = None) -> dict:
    """Build Authorization headers for ImageGen from env key or incoming request."""
    headers = {}
    auth = os.getenv("IMG_API_KEY", "")
    if auth:
        headers["Authorization"] = f"Bearer {auth}"
    elif request is not None:
        incoming = request.headers.get("Authorization", "")
        if incoming:
            headers["Authorization"] = incoming
    return headers


def _rewrite_generated_image_urls(body_json: dict, request: Request) -> None:
    """Rewrite backend file URLs to orchestrator-hosted passthrough URLs."""
    if not isinstance(body_json, dict):
        return
    data = body_json.get("data")
    if not isinstance(data, list):
        return

    host = request.headers.get("host", "")
    if not host:
        return
    public_base = f"{request.url.scheme}://{host}"
    marker = "/v1/images/files/"

    for item in data:
        if not isinstance(item, dict):
            continue
        raw_url = item.get("url")
        if not isinstance(raw_url, str) or not raw_url:
            continue

        parsed = urlparse(raw_url)
        path = parsed.path if parsed.path else raw_url
        if marker not in path:
            continue
        suffix = path.split(marker, 1)[1].lstrip("/")
        item["url"] = f"{public_base}{marker}{suffix}"


_docker_client = None


def _get_docker_client():
    """Create or return a cached docker SDK client."""
    global _docker_client
    if _docker_client is None:
        import docker  # lazy import: orchestrator still works if docker-sdk is absent

        _docker_client = docker.DockerClient(
            base_url=os.getenv("DOCKER_HOST", "unix:///var/run/docker.sock")
        )
    return _docker_client


def _stop_container_if_running(container_name: str) -> bool:
    """Stop a container only when it is currently running."""
    client = _get_docker_client()
    c = client.containers.get(container_name)
    c.reload()
    if c.status == "running":
        c.stop(timeout=20)
        return True
    return False


def _start_container_if_stopped(container_name: str) -> bool:
    """Start a container only when it is currently stopped."""
    client = _get_docker_client()
    c = client.containers.get(container_name)
    c.reload()
    if c.status != "running":
        c.start()
        return True
    return False


_IMAGE_MODEL_ALIASES = {
    "stable-diffusion-2-1": "stable-diffusion-2-1",
    "stable-diffusion-v2-1": "stable-diffusion-2-1",
    "stable-diffusion-v2.1": "stable-diffusion-2-1",
    "stable-diffusion-2.1": "stable-diffusion-2-1",
    "stable-diffusion-v1-5": "stable-diffusion-2-1",
    "stable-diffusion-v1.5": "stable-diffusion-2-1",
    "stable-diffusion-1-5": "stable-diffusion-2-1",
    "stable-diffusion-1.5": "stable-diffusion-2-1",
    "sd1.5": "stable-diffusion-2-1",
    "sd-1.5": "stable-diffusion-2-1",
    "sd2.1": "stable-diffusion-2-1",
    "sd-2.1": "stable-diffusion-2-1",
    "dall-e-2": "stable-diffusion-2-1",
    "dall-e-3": "stable-diffusion-2-1",
    "gpt-image-1": "stable-diffusion-2-1",
    "gpt-image-1.5": "stable-diffusion-2-1",
}


def _normalize_img_generate_body(raw_body: bytes) -> tuple[bytes, Optional[str]]:
    """Force ImageGen model to stable-diffusion-2-1 for legacy callers."""
    try:
        payload = json.loads(raw_body.decode("utf-8")) if raw_body else {}
    except Exception:
        return raw_body, None

    if not isinstance(payload, dict):
        return raw_body, None

    model = str(payload.get("model", "")).strip()
    if not model:
        payload["model"] = "stable-diffusion-2-1"
        return json.dumps(payload).encode("utf-8"), "stable-diffusion-2-1"

    model_l = model.lower()
    normalized = _IMAGE_MODEL_ALIASES.get(model_l, model)
    if normalized == model and (
        model_l.startswith("dall-e")
        or model_l.startswith("gpt-image")
        or (model_l.startswith("stable-diffusion") and model_l != "stable-diffusion-2-1")
    ):
        normalized = "stable-diffusion-2-1"
    payload["model"] = normalized
    return json.dumps(payload).encode("utf-8"), normalized


async def _proxy_image_generation_request(
    request: Request,
    *,
    attach_timing: bool,
):
    """Proxy image generation request with model normalization and timing annotations."""
    t0 = _now()
    raw_body = await request.body()
    read_body_ms = _ms_since(t0)

    t_norm = _now()
    body, normalized_model = _normalize_img_generate_body(raw_body)
    normalize_ms = _ms_since(t_norm)
    if normalized_model:
        print(f"[img] normalized request model -> {normalized_model}")

    # Validate required prompt before attempting NPU arbitration. This keeps
    # invalid requests fast and avoids long waits when the accelerator is busy.
    try:
        parsed = json.loads(body.decode("utf-8")) if body else {}
    except Exception:
        parsed = None
    if not isinstance(parsed, dict):
        return JSONResponse(
            content=_error_body("Invalid JSON", "invalid_request_error"),
            status_code=400,
        )
    prompt = parsed.get("prompt")
    if not isinstance(prompt, str) or not prompt.strip():
        return JSONResponse(
            content=_error_body(
                "'prompt' field is required and must be non-empty",
                "invalid_request_error",
            ),
            status_code=400,
        )

    headers = {"Content-Type": "application/json", **_imagegen_headers(request)}
    upstream_ms = 0.0
    arbitration_ms = 0.0
    npu_wait_ms = 0.0
    try:
        async with acquire_npu_slot("imagegen") as npu_meta:
            npu_wait_ms = float(npu_meta.get("wait_ms", 0.0))
            t_up = _now()
            r = await http_client_img.post(
                f"{IMG_URL}/v1/images/generations",
                content=body,
                headers=headers,
            )
            upstream_ms = _ms_since(t_up)

            if r.status_code >= 500 and "rc=256" in r.text and IMG_I2T_ARBITRATION_ENABLED:
                t_arb = _now()
                stopped = False
                try:
                    stopped = await asyncio.to_thread(_stop_container_if_running, I2T_CONTAINER_NAME)
                    if stopped:
                        print(f"[img] Stopped {I2T_CONTAINER_NAME} for imagegen retry")
                        await asyncio.sleep(2.0)
                    retry = await http_client_img.post(
                        f"{IMG_URL}/v1/images/generations",
                        content=body,
                        headers=headers,
                    )
                    r = retry
                except Exception as exc:
                    print(f"[img] Retry arbitration failed: {exc}")
                finally:
                    if stopped:
                        try:
                            await asyncio.to_thread(_start_container_if_stopped, I2T_CONTAINER_NAME)
                            print(f"[img] Started {I2T_CONTAINER_NAME} after imagegen retry")
                            await wait_for_i2t_ready(timeout_sec=30.0)
                        except Exception as exc:
                            print(f"[img] Failed to start {I2T_CONTAINER_NAME}: {exc}")
                arbitration_ms = _ms_since(t_arb)
            elif r.status_code >= 500 and "rc=256" in r.text:
                print("[img] rc=256 detected; I2T arbitration is disabled (IMG_I2T_ARBITRATION_ENABLED=0)")
    except NpuBusyError as exc:
        return JSONResponse(
            content=_error_body(str(exc), "npu_busy"),
            status_code=429,
            headers={"Retry-After": "2"},
        )

    try:
        body_json = r.json()
    except Exception:
        return Response(
            content=r.content,
            status_code=r.status_code,
            headers={"content-type": r.headers.get("content-type", "application/octet-stream")},
        )

    _rewrite_generated_image_urls(body_json, request)

    if isinstance(body_json, dict):
        upstream_header_timing = {
            "request_total_ms": _header_float(r.headers, "X-Request-Total-Ms"),
            "process_time_ms": _header_float(r.headers, "X-Process-Time-Ms"),
            "engine_total_ms": _header_float(r.headers, "X-Engine-Total-Ms"),
            "ensure_ready_ms": _header_float(r.headers, "X-Ensure-Ready-Ms"),
            "parse_request_ms": _header_float(r.headers, "X-Parse-Request-Ms"),
            "cache_lookup_ms": _header_float(r.headers, "X-Cache-Lookup-Ms"),
            "base64_encode_ms": _header_float(r.headers, "X-Base64-Encode-Ms"),
            "cache_hit_count": _header_float(r.headers, "X-Cache-Hit"),
        }
        upstream_timing = body_json.get("x_timing", {})
        if not isinstance(upstream_timing, dict):
            upstream_timing = {}
        for t_key in (
            "request_total_ms",
            "ensure_ready_ms",
            "parse_request_ms",
            "cache_lookup_ms",
            "base64_encode_ms",
            "cache_hit_count",
        ):
            _merge_numeric_timing(upstream_timing, t_key, upstream_header_timing.get(t_key))

        engine_timing = upstream_timing.get("engine", {})
        if not isinstance(engine_timing, dict):
            engine_timing = {}
        _merge_numeric_timing(engine_timing, "total_ms", upstream_header_timing.get("engine_total_ms"))
        process_ms = body_json.get("x_process_time_ms")
        process_ms_num = process_ms if isinstance(process_ms, (int, float)) else None
        cur_engine_total = engine_timing.get("total_ms")
        cur_engine_total_num = cur_engine_total if isinstance(cur_engine_total, (int, float)) else None
        if process_ms_num is not None and process_ms_num > 0 and (
            cur_engine_total_num is None or cur_engine_total_num <= 0
        ):
            engine_timing["total_ms"] = round(float(process_ms_num), 3)
        if engine_timing:
            upstream_timing["engine"] = engine_timing

        if upstream_timing:
            body_json["x_timing"] = upstream_timing
        _merge_numeric_timing(body_json, "x_process_time_ms", upstream_header_timing.get("process_time_ms"))

    if attach_timing and isinstance(body_json, dict):
        body_json["x_orchestrator_timing"] = {
            "read_body_ms": read_body_ms,
            "normalize_body_ms": normalize_ms,
            "npu_wait_ms": npu_wait_ms,
            "upstream_ms": upstream_ms,
            "arbitration_ms": arbitration_ms,
            "request_total_ms": _ms_since(t0),
            "upstream_request_total_ms": _header_float(r.headers, "X-Request-Total-Ms"),
            "upstream_process_time_ms": _header_float(r.headers, "X-Process-Time-Ms"),
            "upstream_engine_total_ms": _header_float(r.headers, "X-Engine-Total-Ms"),
        }

    response = JSONResponse(content=body_json, status_code=r.status_code)
    response.headers["X-Npu-Wait-Ms"] = str(npu_wait_ms)
    if isinstance(body_json, dict):
        for header_name in (
            "X-Request-Total-Ms",
            "X-Process-Time-Ms",
            "X-Engine-Total-Ms",
            "X-Ensure-Ready-Ms",
            "X-Parse-Request-Ms",
            "X-Cache-Lookup-Ms",
            "X-Base64-Encode-Ms",
            "X-Cache-Hit",
        ):
            if header_name in r.headers:
                response.headers[f"X-Upstream-{header_name[2:]}"] = r.headers[header_name]
    return response


@router.get("/api/img/models")
async def img_models(request: Request):
    """Return ImageGen models, honoring API-key auth when configured."""
    headers = _imagegen_headers(request)
    if not headers:
        models = await img_client.models.list()
        return JSONResponse(content=models.model_dump())

    r = await http_client_default.get(f"{IMG_URL}/v1/models", headers=headers)
    return JSONResponse(content=r.json(), status_code=r.status_code)


@router.get("/api/img/params")
async def img_params(request: Request):
    """Return ImageGen generation parameter metadata."""
    r = await http_client_default.get(
        f"{IMG_URL}/v1/images/generations/params",
        headers=_imagegen_headers(request),
    )
    return JSONResponse(content=r.json(), status_code=r.status_code)


@router.post("/api/img/generate")
async def img_generate(request: Request):
    """Helper API route for image generation with orchestrator timing fields."""
    return await _proxy_image_generation_request(request, attach_timing=True)


@router.post("/v1/images/generations")
async def openai_images_generations(request: Request):
    """OpenAI-compatible image generation proxy route."""
    return await _proxy_image_generation_request(request, attach_timing=False)


@router.post("/v1/images/edits")
async def openai_images_edits(request: Request):
    """OpenAI-compatible image edits proxy route."""
    try:
        npu_meta, release_npu = await acquire_npu_slot("imagegen-edits")
    except TypeError:
        # Keep compatibility with contextmanager form below if direct unpack is unsupported.
        npu_meta = {"wait_ms": 0.0}
        release_npu = lambda: None
    except NpuBusyError as exc:
        return JSONResponse(
            content=_error_body(str(exc), "npu_busy"),
            status_code=429,
            headers={"Retry-After": "2"},
        )
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{IMG_URL}/v1/images/edits",
        client=http_client_stream,
        extra_headers=_imagegen_headers(request),
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.post("/v1/images/variations")
async def openai_images_variations(request: Request):
    """OpenAI-compatible image variations proxy route."""
    try:
        npu_meta, release_npu = await acquire_npu_slot("imagegen-variations")
    except TypeError:
        npu_meta = {"wait_ms": 0.0}
        release_npu = lambda: None
    except NpuBusyError as exc:
        return JSONResponse(
            content=_error_body(str(exc), "npu_busy"),
            status_code=429,
            headers={"Retry-After": "2"},
        )
    response = await _proxy_raw_request(
        request,
        method="POST",
        upstream_url=f"{IMG_URL}/v1/images/variations",
        client=http_client_stream,
        extra_headers=_imagegen_headers(request),
        on_close=release_npu,
    )
    if hasattr(response, "headers"):
        response.headers["X-Npu-Wait-Ms"] = str(float(npu_meta.get("wait_ms", 0.0)))
    return response


@router.get("/v1/images/files/{image_id:path}")
async def openai_images_files(image_id: str, request: Request):
    """Serve generated image files through orchestrator passthrough."""
    return await _proxy_raw_request(
        request,
        method="GET",
        upstream_url=f"{IMG_URL}/v1/images/files/{image_id}",
        client=http_client_stream,
        extra_headers=_imagegen_headers(request),
    )
