#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# System/UI routes.
# ---------------------------------------------------------------------

import asyncio
from pathlib import Path

from fastapi import APIRouter
from fastapi.responses import HTMLResponse

from .common import _ms_since, _now
from .context import I2T_URL, IMG_URL, STT_URL, TG_URL, TTS_URL, http_client_default

router = APIRouter()

STATIC_DIR = Path(__file__).resolve().parent.parent / "static"


@router.get("/", response_class=HTMLResponse)
async def index():
    """Serve the orchestrator SPA index page."""
    return HTMLResponse(content=(STATIC_DIR / "index.html").read_text(encoding="utf-8"))


@router.get("/health")
async def orchestrator_health():
    """Liveness endpoint for orchestrator process health."""
    return {
        "status": "ok",
        "service": "orchestrator",
        "version": "2.0.0",
    }


async def _check_health(name: str, url: str) -> dict:
    """Probe backend liveness/readiness and normalize status payload."""
    t0 = _now()
    try:
        health_resp = await http_client_default.get(f"{url}/health", timeout=3.0)
        health_data = health_resp.json() if health_resp.status_code == 200 else {}

        ready_status = "unsupported"
        ready_http_code = 0
        ready_detail: dict | str = {}
        try:
            ready_resp = await http_client_default.get(f"{url}/ready", timeout=3.0)
            ready_http_code = ready_resp.status_code
            ready_detail = ready_resp.json() if ready_resp.status_code in (200, 503) else {}
            if ready_resp.status_code == 200:
                ready_status = "ready"
            elif ready_resp.status_code == 503:
                ready_status = "not_ready"
            elif ready_resp.status_code in (404, 405):
                ready_status = "unsupported"
            else:
                ready_status = "error"
        except Exception as ready_exc:
            ready_detail = str(ready_exc)
            ready_status = "unreachable"

        normalized_status = "ok" if health_resp.status_code == 200 else "error"
        if normalized_status == "ok" and ready_status == "not_ready":
            normalized_status = "not_ready"
        return {
            "name": name,
            "url": url,
            "status": normalized_status,
            "http_code": health_resp.status_code,
            "detail": health_data,
            "ready_status": ready_status,
            "ready_http_code": ready_http_code,
            "ready_detail": ready_detail,
            "latency_ms": _ms_since(t0),
        }
    except Exception as e:
        return {
            "name": name,
            "url": url,
            "status": "unreachable",
            "http_code": 0,
            "detail": str(e),
            "ready_status": "unreachable",
            "ready_http_code": 0,
            "ready_detail": str(e),
            "latency_ms": _ms_since(t0),
        }


@router.get("/api/status")
async def get_status():
    """Aggregate health status from all backend services."""
    t0 = _now()
    results = await asyncio.gather(
        _check_health("Text-Generation", TG_URL),
        _check_health("Speech-To-Text", STT_URL),
        _check_health("Text-To-Speech", TTS_URL),
        _check_health("Image-Generation", IMG_URL),
        _check_health("Image-To-Text", I2T_URL),
    )
    services = list(results)
    overall_health = "ok" if all(s.get("http_code") == 200 for s in services) else "degraded"
    overall_ready = all(
        (s.get("status") == "ok") and (s.get("ready_status") in ("ready", "unsupported"))
        for s in services
    )
    return {
        "services": services,
        "overall_health": overall_health,
        "overall_ready": overall_ready,
        "x_timing": {"request_total_ms": _ms_since(t0)},
    }
