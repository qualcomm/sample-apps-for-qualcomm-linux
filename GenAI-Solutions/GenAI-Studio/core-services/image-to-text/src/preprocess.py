#!/usr/bin/env python3
# ---------------------------------------------------------------------
# Copyright (c) 2024-2025 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
"""
Preprocess an image for Qwen2.5-VL edge runtime.

Usage:
    python3 preprocess.py <image_source> <output_dir> [model_dir_or_id]

image_source supports:
    - local file path
    - https URL
    - data URL (data:image/...;base64,...)
"""
import base64
import io
import os
import sys
import urllib.request

MAX_IMAGE_BYTES = int(os.getenv("I2T_MAX_IMAGE_BYTES", str(20 * 1024 * 1024)))
MAX_IMAGE_PIXELS = int(os.getenv("I2T_MAX_IMAGE_PIXELS", str(40_000_000)))
SUPPORTED_FORMATS = {"JPEG", "PNG", "WEBP", "GIF"}


def _decode_data_url(source: str) -> bytes:
    if "," not in source:
        raise RuntimeError("invalid data URL: missing comma")
    meta, payload = source.split(",", 1)
    if ";base64" not in meta.lower():
        raise RuntimeError("data URL must be base64-encoded")
    try:
        decoded = base64.b64decode(payload, validate=False)
    except Exception as exc:
        raise RuntimeError(f"invalid base64 data URL: {exc}") from exc
    if len(decoded) > MAX_IMAGE_BYTES:
        raise RuntimeError(f"image exceeds max size: {len(decoded)} > {MAX_IMAGE_BYTES} bytes")
    return decoded


def _load_image_processor(model_id: str):
    from transformers import Qwen2VLImageProcessor

    if model_id:
        try:
            return Qwen2VLImageProcessor.from_pretrained(
                model_id, use_fast=False, local_files_only=True
            )
        except Exception:
            pass

    return Qwen2VLImageProcessor()


def _load_image_bytes(image_source: str) -> bytes:
    source = (image_source or "").strip()
    if not source:
        raise RuntimeError("image source is empty")

    if source.startswith("data:"):
        return _decode_data_url(source)

    if source.startswith("http://"):
        raise RuntimeError("only https:// URLs are supported")

    if source.startswith("https://"):
        try:
            with urllib.request.urlopen(source, timeout=60) as resp:
                data = resp.read(MAX_IMAGE_BYTES + 1)
                if len(data) > MAX_IMAGE_BYTES:
                    raise RuntimeError(f"image exceeds max size: > {MAX_IMAGE_BYTES} bytes")
                return data
        except Exception as exc:
            raise RuntimeError(f"failed to download image URL: {exc}") from exc

    if not os.path.isfile(source):
        raise RuntimeError(f"local image path not found: {source}")
    if os.path.getsize(source) > MAX_IMAGE_BYTES:
        raise RuntimeError(f"image exceeds max size: {os.path.getsize(source)} > {MAX_IMAGE_BYTES} bytes")
    with open(source, "rb") as f:
        data = f.read()
    # Allow data URL payloads to be passed indirectly via temp file path.
    if data.startswith(b"data:"):
        return _decode_data_url(data.decode("utf-8", errors="replace").strip())
    return data


def preprocess(image_source: str, output_dir: str, model_id: str) -> str:
    import numpy as np
    from PIL import Image

    processor = _load_image_processor(model_id)
    image_bytes = _load_image_bytes(image_source)
    image_raw = Image.open(io.BytesIO(image_bytes))
    img_format = (image_raw.format or "").upper()
    if img_format not in SUPPORTED_FORMATS:
        raise RuntimeError(
            "unsupported image format "
            f"'{img_format or 'unknown'}'; supported: jpeg, png, webp, gif"
        )
    width, height = image_raw.size
    if (width * height) > MAX_IMAGE_PIXELS:
        raise RuntimeError(
            f"image exceeds max pixels: {width}x{height} > {MAX_IMAGE_PIXELS}"
        )
    if img_format == "GIF":
        try:
            image_raw.seek(0)  # use first frame for animated GIF inputs
        except Exception:
            pass
    image = image_raw.convert("RGB")
    image = image.resize((512, 342), Image.BICUBIC)

    inputs = processor(images=[image], return_tensors="np")
    if "pixel_values" not in inputs:
        raise RuntimeError("processor output missing 'pixel_values'")
    pixel_values = np.asarray(inputs["pixel_values"], dtype=np.float32)

    os.makedirs(output_dir, exist_ok=True)
    pv_path = os.path.join(output_dir, "pixel_values.raw")
    pixel_values.tofile(pv_path)
    return os.path.abspath(pv_path)


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(
            "Usage: preprocess.py <image_source> <output_dir> [model_dir_or_id]",
            file=sys.stderr,
        )
        sys.exit(1)

    image_source = sys.argv[1]
    out_dir = sys.argv[2]
    model_id = sys.argv[3] if len(sys.argv) > 3 else os.getenv("I2T_MODEL_ID", "")

    try:
        print(preprocess(image_source, out_dir, model_id))
        sys.exit(0)
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
