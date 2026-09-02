#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Shared configuration and HTTP/OpenAI clients for orchestrator routes.
# ---------------------------------------------------------------------

import os

import httpx
from openai import AsyncOpenAI

# ---------------------------------------------------------------------------
# Service endpoints
# ---------------------------------------------------------------------------
TG_URL = os.getenv("TG_URL", "http://localhost:8088")
STT_URL = os.getenv("STT_URL", "http://localhost:8081")
IMG_URL = os.getenv("IMG_URL", "http://localhost:8084")
I2T_URL = os.getenv("I2T_URL", "http://localhost:8080")
TTS_URL = os.getenv("TTS_URL", "http://localhost:8083")
OPENAI_BASE_URL = os.getenv("OPENAI_BASE_URL", "https://api.openai.com")
OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "")
TG_ORCHESTRATOR_MODEL_ID = os.getenv("TG_ORCHESTRATOR_MODEL_ID", "genie")
TG_DIRECT_MODEL_ID = os.getenv("TG_DIRECT_MODEL_ID", "qwen3-4B")

MODEL_DIR = os.getenv(
    "MODEL_DIR",
    "/opt/I2T_binary/files",
)
UPLOADS_DIR = os.getenv("UPLOADS_DIR", f"{MODEL_DIR}/uploads")
MODEL_ID = os.getenv("VLM_MODEL_ID", "Qwen/Qwen2.5-VL-7B-Instruct")
I2T_CONTAINER_NAME = os.getenv("I2T_CONTAINER_NAME", "Image2Text")
PRELOAD_I2T_PROCESSOR = os.getenv("PRELOAD_I2T_PROCESSOR", "1").lower() not in (
    "0",
    "false",
    "no",
)


# ---------------------------------------------------------------------------
# OpenAI clients
# ---------------------------------------------------------------------------
def _make_client(base_url: str, timeout: float = 120.0, api_key: str = "none") -> AsyncOpenAI:
    """Create OpenAI-compatible async client for a backend service."""
    return AsyncOpenAI(
        base_url=base_url,
        api_key=api_key,
        timeout=timeout,
        http_client=httpx.AsyncClient(timeout=timeout),
    )


tg_client = _make_client(f"{TG_URL}/v1", timeout=120.0)
stt_client = _make_client(f"{STT_URL}/v1", timeout=120.0)
i2t_client = _make_client(f"{I2T_URL}/v1", timeout=600.0)
tts_client = _make_client(f"{TTS_URL}/v1", timeout=120.0)
img_client = _make_client(
    f"{IMG_URL}/v1",
    timeout=300.0,
    api_key=os.getenv("IMG_API_KEY", "none"),
)


# Shared HTTP clients reduce per-request connection setup overhead.
http_client_default = httpx.AsyncClient(timeout=10.0)
http_client_img = httpx.AsyncClient(timeout=300.0)
http_client_i2t = httpx.AsyncClient(timeout=600.0)
http_client_stream = httpx.AsyncClient(timeout=600.0)


async def close_http_clients() -> None:
    """Close shared async HTTP clients during app shutdown."""
    await http_client_default.aclose()
    await http_client_img.aclose()
    await http_client_i2t.aclose()
    await http_client_stream.aclose()
