#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# run.sh – Start the Text-Generation service inside the container.
#
# textgen_server is an HTTP server that defaults to 0.0.0.0:8088
# (override with TG_BIND_HOST/TG_BIND_PORT).
# It exposes an OpenAI-compatible API:
#   POST /v1/chat/completions   – streaming + non-streaming inference
#   GET  /v1/models             – list loaded model
#   GET  /v1/internal/models    – list local model bundles
#   POST /v1/internal/models/load – switch active local model
#   GET  /health                – health check
#   POST /reset_model           – reset dialog state
#   POST /reload_model          – reload with new sampler / max_tokens
#
# Environment variable overrides:
#   TG_MODELS_ROOT  – root directory containing one or more model folders
#                     (default: /opt/genai-studio-models/text-to-text)
#   TG_MODEL_NAME   – active model folder name under TG_MODELS_ROOT
#                     (default: qwen3_4b-genie-w4a16-qualcomm_qcs9075)
#   TG_MODEL_DIR    – explicit active model directory (overrides root+name)
#   GENIE_CONFIG    – explicit path to genie_config.json (overrides TG_MODEL_DIR)
#   BASE_DIR        – explicit --base-dir (default: dirname(GENIE_CONFIG))
#
# Runtime library resolution:
#   Prefer QAIRT 2.45 flat runtime path on target.
# ---------------------------------------------------------------------

set -euo pipefail

TG_MODELS_ROOT="${TG_MODELS_ROOT:-/opt/genai-studio-models/text-to-text}"
TG_MODEL_NAME="${TG_MODEL_NAME:-qwen3_4b-genie-w4a16-qualcomm_qcs9075}"
TG_MODEL_DIR="${TG_MODEL_DIR:-}"
TG_MODEL_ID_OVERRIDE="${TG_MODEL_ID_OVERRIDE:-}"
QAIRT_VERSION_HINT="${QAIRT_VERSION_HINT:-build=2.45.0.260326;runtime=2.45.0.260326}"
TG_BIND_HOST="${TG_BIND_HOST:-0.0.0.0}"
TG_BIND_PORT="${TG_BIND_PORT:-8088}"
TG_INTERNAL_AUTH_ENFORCE="${TG_INTERNAL_AUTH_ENFORCE:-0}"
TG_INTERNAL_API_KEY="${TG_INTERNAL_API_KEY:-tg-internal-placeholder-key}"

DEFAULT_TG_LD_LIBRARY_PATH="/opt/qairt/current/qairt_245_flat_libs:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu"
DEFAULT_TG_ADSP_LIBRARY_PATH="/opt/qairt/current/qairt_245_flat_libs;/usr/lib/rfsa/adsp;/usr/lib/dsp;/dsp;/usr/lib/dsp/cdsp1"

prepend_colon_path() {
    local current="$1"
    local segment="$2"
    if [[ -z "${current}" ]]; then
        echo "${segment}"
        return
    fi
    case ":${current}:" in
        *":${segment}:"*) echo "${current}" ;;
        *) echo "${segment}:${current}" ;;
    esac
}

prepend_semicolon_path() {
    local current="$1"
    local segment="$2"
    if [[ -z "${current}" ]]; then
        echo "${segment}"
        return
    fi
    case ";${current};" in
        *";${segment};"*) echo "${current}" ;;
        *) echo "${segment};${current}" ;;
    esac
}

if [[ -z "${TG_MODEL_DIR}" ]]; then
    if [[ -d "${TG_MODELS_ROOT}/${TG_MODEL_NAME}" ]]; then
        TG_MODEL_DIR="${TG_MODELS_ROOT}/${TG_MODEL_NAME}"
    elif [[ -d "${TG_MODELS_ROOT}" ]]; then
        TG_MODEL_DIR="${TG_MODELS_ROOT}"
    fi
fi

GENIE_CONFIG="${GENIE_CONFIG:-}"
if [[ -z "${GENIE_CONFIG}" ]]; then
    for candidate in \
        "${TG_MODEL_DIR}/genie_config.json" \
        "${TG_MODELS_ROOT}/${TG_MODEL_NAME}/genie_config.json"; do
        if [[ -f "${candidate}" ]]; then
            GENIE_CONFIG="${candidate}"
            break
        fi
    done
    if [[ -z "${GENIE_CONFIG}" ]]; then
        GENIE_CONFIG="${TG_MODEL_DIR}/genie_config.json"
    fi
fi
BASE_DIR="${BASE_DIR:-$(dirname "${GENIE_CONFIG}")}"

if [[ -n "${TG_MODEL_ID_OVERRIDE}" ]]; then
    export TG_MODEL_ID_OVERRIDE
fi
export TG_BIND_HOST
export TG_BIND_PORT
export TG_INTERNAL_AUTH_ENFORCE
export TG_INTERNAL_API_KEY

LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-${DEFAULT_TG_LD_LIBRARY_PATH}}"
ADSP_LIBRARY_PATH="${ADSP_LIBRARY_PATH:-${DEFAULT_TG_ADSP_LIBRARY_PATH}}"

for runtime_dir in \
    "/opt/qairt/current/qairt_245_flat_libs"; do
    if [[ -d "${runtime_dir}" ]]; then
        LD_LIBRARY_PATH="$(prepend_colon_path "${LD_LIBRARY_PATH}" "${runtime_dir}")"
        ADSP_LIBRARY_PATH="$(prepend_semicolon_path "${ADSP_LIBRARY_PATH}" "${runtime_dir}")"
    fi
done

export LD_LIBRARY_PATH
export ADSP_LIBRARY_PATH

APP_BIN="$(command -v textgen_server || true)"
if [[ -z "${APP_BIN}" ]]; then
    APP_BIN="$(command -v llamachat || true)"
fi

echo "============================================================"
echo " Text-Generation service starting"
echo "============================================================"
echo " Service bin   : ${APP_BIN:-<not-found>}"
echo " Models root   : ${TG_MODELS_ROOT}"
echo " Model name    : ${TG_MODEL_NAME}"
echo " Model dir     : ${TG_MODEL_DIR}"
echo " Model id      : ${TG_MODEL_ID_OVERRIDE:-<from-config>}"
echo " QAIRT hint    : ${QAIRT_VERSION_HINT}"
echo " Genie config : ${GENIE_CONFIG}"
echo " Base dir     : ${BASE_DIR}"
echo " Bind host    : ${TG_BIND_HOST}"
echo " Bind port    : ${TG_BIND_PORT}"
echo " Auth enforce : ${TG_INTERNAL_AUTH_ENFORCE}"
echo "============================================================"
echo " LD_LIBRARY_PATH : ${LD_LIBRARY_PATH:-<unset>}"
echo " ADSP_LIBRARY_PATH: ${ADSP_LIBRARY_PATH:-<unset>}"
echo "============================================================"

if [[ -n "${TG_MODELS_ROOT}" && -d "${TG_MODELS_ROOT}" ]]; then
    echo " Available model bundles:"
    find "${TG_MODELS_ROOT}" -maxdepth 3 -type f -name "genie_config.json" 2>/dev/null | sort || true
    echo "============================================================"
fi

if [[ -z "${APP_BIN}" ]]; then
    echo "[run.sh] ERROR: no server binary found (textgen_server/llamachat)"
    exit 1
fi

# Verify the genie config exists before launching
if [[ ! -f "${GENIE_CONFIG}" ]]; then
    echo ""
    echo "[run.sh] ERROR: genie_config.json not found at: ${GENIE_CONFIG}"
    echo "[run.sh] Ensure model directory is mounted and variables are correct:"
    echo "[run.sh]   -e TG_MODELS_ROOT=<root> -e TG_MODEL_NAME=<folder>"
    echo "[run.sh]   or set: -e TG_MODEL_DIR=<dir> -e GENIE_CONFIG=<path>"
    exit 1
fi

if [[ ! -d "${BASE_DIR}" ]]; then
    echo "[run.sh] ERROR: BASE_DIR does not exist: ${BASE_DIR}"
    exit 1
fi

echo "[run.sh] genie_config.json found. Starting ${APP_BIN}..."

exec "${APP_BIN}" \
    --genie-config "${GENIE_CONFIG}" \
    --base-dir "${BASE_DIR}"
