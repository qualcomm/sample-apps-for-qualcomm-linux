#!/bin/bash
# =============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# run.sh – Container entrypoint for Image-To-Text (llamachat VLM service)
#
# Environment variables:
#   MODEL_DIR   Path to VLM model directory (default: /opt/I2T_binary/files)
# =============================================================================

set -euo pipefail

MODEL_DIR="${MODEL_DIR:-/opt/I2T_binary/files}"
QAIRT_FLAT_LIB_DIR="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"
QAIRT_VERSION_HINT="${QAIRT_VERSION_HINT:-build=2.45.0.260326;runtime=/opt/qairt/current/qairt_245_flat_libs}"
I2T_PATCH_JSON_ABS_PATHS="${I2T_PATCH_JSON_ABS_PATHS:-1}"
I2T_IMAGE_ENCODER_CONFIG="${I2T_IMAGE_ENCODER_CONFIG:-}"
I2T_TEXT_ENCODER_CONFIG="${I2T_TEXT_ENCODER_CONFIG:-}"
I2T_TEXT_DECODER_CONFIG="${I2T_TEXT_DECODER_CONFIG:-}"
I2T_INPUTS_DIR="${I2T_INPUTS_DIR:-}"

resolve_model_file() {
    local env_value="$1"
    shift
    if [[ -n "${env_value}" ]]; then
        if [[ -f "${env_value}" ]]; then
            echo "${env_value}"
            return 0
        fi
        if [[ -f "${MODEL_DIR}/${env_value}" ]]; then
            echo "${MODEL_DIR}/${env_value}"
            return 0
        fi
        return 1
    fi
    local cand
    for cand in "$@"; do
        if [[ -f "${MODEL_DIR}/${cand}" ]]; then
            echo "${MODEL_DIR}/${cand}"
            return 0
        fi
    done
    return 1
}

resolve_model_dir() {
    local env_value="$1"
    shift
    if [[ -n "${env_value}" ]]; then
        if [[ -d "${env_value}" ]]; then
            echo "${env_value}"
            return 0
        fi
        if [[ -d "${MODEL_DIR}/${env_value}" ]]; then
            echo "${MODEL_DIR}/${env_value}"
            return 0
        fi
        return 1
    fi
    local cand
    for cand in "$@"; do
        if [[ -d "${MODEL_DIR}/${cand}" ]]; then
            echo "${MODEL_DIR}/${cand}"
            return 0
        fi
    done
    return 1
}

IMAGE_CFG_PATH="$(resolve_model_file "${I2T_IMAGE_ENCODER_CONFIG}" img-enc-htp.json image_encoder.json || true)"
if [[ -z "${IMAGE_CFG_PATH}" ]]; then
    echo "[run.sh] ERROR: image encoder config not found (set I2T_IMAGE_ENCODER_CONFIG or provide img-enc-htp.json/image_encoder.json)"
    exit 1
fi

TEXT_ENC_CFG_PATH="$(resolve_model_file "${I2T_TEXT_ENCODER_CONFIG}" text-encoder.json lut_encoder.json || true)"
if [[ -z "${TEXT_ENC_CFG_PATH}" ]]; then
    echo "[run.sh] ERROR: text encoder config not found (set I2T_TEXT_ENCODER_CONFIG or provide text-encoder.json/lut_encoder.json)"
    exit 1
fi

TEXT_DEC_CFG_PATH="$(resolve_model_file "${I2T_TEXT_DECODER_CONFIG}" text-dec-htp.json text-generator.json || true)"
if [[ -z "${TEXT_DEC_CFG_PATH}" ]]; then
    echo "[run.sh] ERROR: text decoder config not found (set I2T_TEXT_DECODER_CONFIG or provide text-dec-htp.json/text-generator.json)"
    exit 1
fi

INPUTS_DIR_PATH="$(resolve_model_dir "${I2T_INPUTS_DIR}" inputs sample_inputs || true)"
if [[ -z "${INPUTS_DIR_PATH}" ]]; then
    echo "[run.sh] ERROR: inputs directory not found (set I2T_INPUTS_DIR or provide inputs/ or sample_inputs/)"
    exit 1
fi

export I2T_IMAGE_ENCODER_CONFIG="${IMAGE_CFG_PATH}"
export I2T_TEXT_ENCODER_CONFIG="${TEXT_ENC_CFG_PATH}"
export I2T_TEXT_DECODER_CONFIG="${TEXT_DEC_CFG_PATH}"
export I2T_INPUTS_DIR="${INPUTS_DIR_PATH}"

if [[ "${I2T_PATCH_JSON_ABS_PATHS}" == "1" || "${I2T_PATCH_JSON_ABS_PATHS}" == "true" || "${I2T_PATCH_JSON_ABS_PATHS}" == "TRUE" ]]; then
    python3 - "${MODEL_DIR}" "${IMAGE_CFG_PATH}" "${TEXT_ENC_CFG_PATH}" "${TEXT_DEC_CFG_PATH}" "${MODEL_DIR}/genie_config.json" <<'PY'
import json
import os
import sys
from pathlib import Path

model_dir = Path(sys.argv[1]).resolve()
cfg_files = [Path(p) for p in sys.argv[2:]]
path_suffixes = (".bin", ".raw", ".json", ".so", ".dat", ".txt")

def patch_node(node):
    changed = False
    if isinstance(node, dict):
        for k, v in list(node.items()):
            nv, c = patch_node(v)
            if c:
                node[k] = nv
                changed = True
        return node, changed
    if isinstance(node, list):
        for i, v in enumerate(node):
            nv, c = patch_node(v)
            if c:
                node[i] = nv
                changed = True
        return node, changed
    if isinstance(node, str):
        s = node.strip()
        low = s.lower()
        if not s or s.startswith(("/", "http://", "https://", "data:")):
            return node, False
        if not low.endswith(path_suffixes):
            return node, False
        cand = (model_dir / s).resolve()
        if cand.exists():
            return str(cand), True
        return node, False
    return node, False

for cfg in cfg_files:
    if not cfg.exists():
        continue
    try:
        data = json.loads(cfg.read_text())
    except Exception:
        continue
    patched, changed = patch_node(data)
    if changed:
        cfg.write_text(json.dumps(patched, indent=2))
PY
fi

if [[ -d "${QAIRT_FLAT_LIB_DIR}" ]]; then
    export LD_LIBRARY_PATH="${QAIRT_FLAT_LIB_DIR}:${MODEL_DIR}:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH:-}"
    export ADSP_LIBRARY_PATH="${QAIRT_FLAT_LIB_DIR};${MODEL_DIR};/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1"
else
    echo "[run.sh] WARNING: QAIRT_FLAT_LIB_DIR not found: ${QAIRT_FLAT_LIB_DIR}"
    echo "[run.sh] Falling back to model-bundled libraries only."
    export LD_LIBRARY_PATH="${MODEL_DIR}:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu:${LD_LIBRARY_PATH:-}"
    export ADSP_LIBRARY_PATH="${MODEL_DIR};/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1"
fi

# Ensure uploads directory exists
mkdir -p "${MODEL_DIR}/uploads"

echo "============================================================"
echo " Image-To-Text (llamachat VLM) starting"
echo "============================================================"
echo " Model dir        : ${MODEL_DIR}"
echo " QAIRT hint       : ${QAIRT_VERSION_HINT}"
echo " Port             : 8080"
echo " LD_LIBRARY_PATH  : ${LD_LIBRARY_PATH}"
echo " ADSP_LIBRARY_PATH: ${ADSP_LIBRARY_PATH}"
echo "============================================================"

# Verify model directory and libGenie.so are present
if [[ ! -f "${MODEL_DIR}/libGenie.so" ]]; then
    echo ""
    echo "[run.sh] ERROR: libGenie.so not found at: ${MODEL_DIR}"
    echo "[run.sh] Mount the VLM model directory at runtime:"
    echo "[run.sh]   docker run -v ${MODEL_DIR}:${MODEL_DIR} ..."
    exit 1
fi

if [[ -d "${QAIRT_FLAT_LIB_DIR}" ]]; then
    for req in libQnnHtp.so libQnnSystem.so; do
        if [[ ! -e "${QAIRT_FLAT_LIB_DIR}/${req}" ]]; then
            echo "[run.sh] ERROR: required QNN runtime library missing: ${QAIRT_FLAT_LIB_DIR}/${req}"
            exit 1
        fi
    done
else
    for req in libQnnHtp.so libQnnSystem.so; do
        if [[ ! -e "${MODEL_DIR}/${req}" ]]; then
            echo "[run.sh] ERROR: required QNN runtime library missing: ${MODEL_DIR}/${req}"
            exit 1
        fi
    done
fi

if [[ ! -e /opt/host-libs/libcdsprpc.so.1 && \
      ! -e /opt/host-libs/libcdsprpc.so && \
      ! -e /opt/host-libs/libcdsprpc.so.1.0.0 && \
      ! -e /usr/lib/libcdsprpc.so.1 && \
      ! -e /usr/lib/libcdsprpc.so && \
      ! -e /usr/lib/libcdsprpc.so.1.0.0 && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1 && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so && \
      ! -e /usr/lib/aarch64-linux-gnu/libcdsprpc.so.1.0.0 ]]; then
    echo "[run.sh] ERROR: libcdsprpc not found in /opt/host-libs or system lib paths."
    echo "[run.sh] Check HOST_RPC_LIB_DIR in docker-compose/.env."
    exit 1
fi

echo "[run.sh] Model directory found. Starting llamachat VLM..."

I2T_RESTART_BACKOFF_SEC="${I2T_RESTART_BACKOFF_SEC:-1}"
I2T_MAX_RESTARTS="${I2T_MAX_RESTARTS:-0}"
I2T_VISION_WARMUP_ENABLED="${I2T_VISION_WARMUP_ENABLED:-1}"
I2T_WARMUP_HEALTH_TIMEOUT_SEC="${I2T_WARMUP_HEALTH_TIMEOUT_SEC:-90}"
I2T_WARMUP_RETRIES="${I2T_WARMUP_RETRIES:-2}"
I2T_WARMUP_PROMPT="${I2T_WARMUP_PROMPT:-Describe this image in one short sentence.}"
# 1x1 transparent PNG
I2T_WARMUP_IMAGE_DATA_URL="${I2T_WARMUP_IMAGE_DATA_URL:-data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+tmN8AAAAASUVORK5CYII=}"

wait_for_health() {
    local pid="$1"
    local timeout_s="$2"
    local elapsed=0
    while [[ "${elapsed}" -lt "${timeout_s}" ]]; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            return 1
        fi
        local code
        code="$(curl -s -o /dev/null -w "%{http_code}" --max-time 2 http://127.0.0.1:8080/health || true)"
        if [[ "${code}" == "200" ]]; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

run_vision_warmup() {
    local payload
    payload="$(cat <<JSON
{
  "model": "qwen2.5-vl-7b-instruct",
  "stream": false,
  "input": [
    {
      "role": "user",
      "content": [
        {"type": "input_image", "image_url": "${I2T_WARMUP_IMAGE_DATA_URL}"},
        {"type": "input_text", "text": "${I2T_WARMUP_PROMPT}"}
      ]
    }
  ]
}
JSON
)"

    local code
    code="$(curl -sS -o /tmp/i2t_warmup_response.json \
      -w "%{http_code}" \
      --max-time 150 \
      -X POST http://127.0.0.1:8080/v1/responses \
      -H "Content-Type: application/json" \
      -H "X-Session-Id: __warmup__" \
      -d "${payload}" || true)"

    if [[ "${code}" != "200" ]]; then
        echo "[run.sh] WARN: warmup /v1/responses returned status=${code}"
        if [[ -s /tmp/i2t_warmup_response.json ]]; then
            head -c 400 /tmp/i2t_warmup_response.json || true
            echo ""
        fi
        return 1
    fi

    curl -s -o /dev/null --max-time 20 \
      -X POST http://127.0.0.1:8080/v1/session/reset \
      -H "X-Session-Id: __warmup__" || true
    return 0
}

restart_count=0
while true; do
    /usr/bin/llamachat "${MODEL_DIR}" &
    server_pid=$!

    if ! wait_for_health "${server_pid}" "${I2T_WARMUP_HEALTH_TIMEOUT_SEC}"; then
        echo "[run.sh] WARN: llamachat failed health startup check"
        if kill -0 "${server_pid}" 2>/dev/null; then
            kill "${server_pid}" 2>/dev/null || true
            wait "${server_pid}" || true
        fi
        rc=1
    elif [[ "${I2T_VISION_WARMUP_ENABLED}" == "1" || "${I2T_VISION_WARMUP_ENABLED}" == "true" || "${I2T_VISION_WARMUP_ENABLED}" == "TRUE" ]]; then
        warmup_ok=0
        attempt=1
        while [[ "${attempt}" -le "${I2T_WARMUP_RETRIES}" ]]; do
            if ! kill -0 "${server_pid}" 2>/dev/null; then
                echo "[run.sh] WARN: llamachat exited during warmup attempt=${attempt}"
                break
            fi
            if run_vision_warmup; then
                warmup_ok=1
                echo "[run.sh] Warmup vision call succeeded."
                break
            fi
            echo "[run.sh] WARN: warmup attempt ${attempt}/${I2T_WARMUP_RETRIES} failed"
            attempt=$((attempt + 1))
            sleep 1
        done

        if [[ "${warmup_ok}" -ne 1 ]]; then
            echo "[run.sh] WARN: warmup failed; recycling llamachat process."
            if kill -0 "${server_pid}" 2>/dev/null; then
                kill "${server_pid}" 2>/dev/null || true
                wait "${server_pid}" || true
            fi
            rc=1
        else
            if wait "${server_pid}"; then
                rc=0
            else
                rc=$?
            fi
        fi
    else
        if wait "${server_pid}"; then
            rc=0
        else
            rc=$?
        fi
    fi

    if [[ "${rc}" -eq 0 ]]; then
        rc=0
    fi

    restart_count=$((restart_count + 1))
    echo "[run.sh] WARN: llamachat exited (code=${rc}). restart_count=${restart_count}"

    if [[ "${I2T_MAX_RESTARTS}" -gt 0 && "${restart_count}" -ge "${I2T_MAX_RESTARTS}" ]]; then
        echo "[run.sh] ERROR: reached I2T_MAX_RESTARTS=${I2T_MAX_RESTARTS}; exiting."
        exit "${rc}"
    fi

    sleep "${I2T_RESTART_BACKOFF_SEC}"
done
