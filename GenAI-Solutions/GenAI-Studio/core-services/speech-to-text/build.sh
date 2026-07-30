#!/bin/bash
# =============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
#
# build.sh – Build speech-to-text:latest Docker image
#
# This script orchestrates the full 5-step build pipeline:
#   1. Ensure repo-root qairt-sdk/ exists for Dockerfile.build-base
#   2. Ensure genai-build-base:latest exists (cmake + gcc + Qualcomm headers)
#   3. Ensure ubuntu-runtime:24.04 exists (minimal flat runtime base)
#   4. Prepare whisper_sdk/ build context (minimal slice from WHISPER_SDK_ROOT)
#   5. Build speech-to-text:latest via multi-stage Docker build
#
# Usage:
#   WHISPER_SDK_ROOT=/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk bash build.sh [--clean] [--no-cache]
#
# Environment variables:
#   WHISPER_SDK_ROOT   Path to the Whisper SDK root (required for step 3)
#   FORCE_REBUILD      Set to 1 to force rebuild of base images
#   STT_PREPARE_MODEL  Set to 1 to run legacy AIHub prep helper (default: 0)
#   STT_MODEL_DIR      Output model directory for AIHub downloads
#
# Example:
#   WHISPER_SDK_ROOT=/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk \
#     bash build.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
resolve_repo_root() {
    local d="${SCRIPT_DIR}"
    while [[ "${d}" != "/" ]]; do
        if [[ -f "${d}/Dockerfile.build-base" && -f "${d}/docker-compose.yml" ]]; then
            echo "${d}"
            return 0
        fi
        d="$(dirname "${d}")"
    done
    return 1
}
REPO_ROOT="$(resolve_repo_root || true)"
if [[ -z "${REPO_ROOT}" ]]; then
    echo "[build.sh] ERROR: failed to locate repository root from ${SCRIPT_DIR}" >&2
    exit 1
fi

WHISPER_SDK_ROOT="${WHISPER_SDK_ROOT:-}"
FORCE_REBUILD="${FORCE_REBUILD:-0}"
STT_PREPARE_MODEL="${STT_PREPARE_MODEL:-0}"
STT_MODEL_DIR="${STT_MODEL_DIR:-/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075}"
NO_CACHE=""
CLEAN=0

# ---- Argument parsing -------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)      CLEAN=1;          shift ;;
        --no-cache)   NO_CACHE="--no-cache"; shift ;;
        --force)      FORCE_REBUILD=1;  shift ;;
        --help|-h)
            sed -n '3,25p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "[build.sh] Unknown argument: $1" >&2; exit 1 ;;
    esac
done

echo "============================================================"
echo " Speech-To-Text (speech-to-text) Docker Build"
echo "============================================================"
echo " Repo root       : ${REPO_ROOT}"
echo " WHISPER_SDK_ROOT: ${WHISPER_SDK_ROOT:-<not set>}"
echo " STT_PREPARE_MODEL: ${STT_PREPARE_MODEL}"
echo " STT_MODEL_DIR    : ${STT_MODEL_DIR}"
echo "============================================================"

# ─────────────────────────────────────────────────────────────────────────────
# Prep: Ensure repo-root qairt-sdk/ exists for Dockerfile.build-base
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "[1/5] Checking repo-root qairt-sdk/ slice..."

QAIRT_SDK_SLICE="${REPO_ROOT}/qairt-sdk"
if [[ "${FORCE_REBUILD}" == "1" ]] || [[ ! -d "${QAIRT_SDK_SLICE}" ]]; then
    echo "      qairt-sdk/ missing or FORCE_REBUILD=1. Preparing from local QAIRT install..."
    env \
        FORCE_DOWNLOAD="${FORCE_REBUILD}" \
        bash "${REPO_ROOT}/scripts/download-qairt-sdk.sh" --service base
else
    echo "      ✓ qairt-sdk/ already exists"
fi

if [[ ! -d "${QAIRT_SDK_SLICE}/include/Genie" ]] || [[ ! -d "${QAIRT_SDK_SLICE}/include/QNN" ]]; then
    echo "[build.sh] ERROR: qairt-sdk slice is incomplete under ${QAIRT_SDK_SLICE}" >&2
    exit 1
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Ensure genai-build-base:latest exists (C++ compiler + Qualcomm headers)
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "[2/5] Checking genai-build-base:latest..."

if [[ "${FORCE_REBUILD}" == "1" ]] || ! docker image inspect genai-build-base:latest &>/dev/null; then
    echo "      genai-build-base:latest not found. Building from Dockerfile.build-base..."
    DOCKER_BUILDKIT=1 docker build \
        --progress=plain \
        -f "${REPO_ROOT}/Dockerfile.build-base" \
        -t genai-build-base:latest \
        "${REPO_ROOT}"
    echo "      ✓ genai-build-base:latest built"
else
    echo "      ✓ genai-build-base:latest already exists"
    docker images genai-build-base:latest --format "        ID={{.ID}}  Size={{.Size}}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Ensure ubuntu-runtime:24.04 exists (minimal runtime base)
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "[3/5] Checking ubuntu-runtime:24.04..."

if [[ "${FORCE_REBUILD}" == "1" ]] || ! docker image inspect ubuntu-runtime:24.04 &>/dev/null; then
    echo "      ubuntu-runtime:24.04 not found. Creating from scripts/create-runtime-base.sh..."
    bash "${REPO_ROOT}/scripts/create-runtime-base.sh"
    echo "      ✓ ubuntu-runtime:24.04 created"
else
    echo "      ✓ ubuntu-runtime:24.04 already exists"
    docker images ubuntu-runtime:24.04 --format "        ID={{.ID}}  Size={{.Size}}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Prepare whisper_sdk/ build context
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "[4/5] Preparing whisper_sdk/ build context..."

WHISPER_SDK_DEST="${SCRIPT_DIR}/whisper_sdk"
LEGACY_WHISPER_SDK_DEST="${SCRIPT_DIR}/whisper-sdk"

if [[ "${CLEAN}" == "1" ]]; then
    echo "      Cleaning ${WHISPER_SDK_DEST}..."
    rm -rf "${WHISPER_SDK_DEST}"
    rm -rf "${LEGACY_WHISPER_SDK_DEST}"
fi

if [[ -d "${LEGACY_WHISPER_SDK_DEST}" && ! -d "${WHISPER_SDK_DEST}" ]]; then
    echo "      Migrating legacy ${LEGACY_WHISPER_SDK_DEST} -> ${WHISPER_SDK_DEST}"
    mv "${LEGACY_WHISPER_SDK_DEST}" "${WHISPER_SDK_DEST}"
fi

if [[ -d "${WHISPER_SDK_DEST}" && "${FORCE_REBUILD}" != "1" ]]; then
    echo "      whisper_sdk/ already exists. Skipping."
else
    if [[ -z "${WHISPER_SDK_ROOT}" ]]; then
        echo "[build.sh] ERROR: WHISPER_SDK_ROOT is not set."
        echo "           Set it to the path of the Whisper SDK root directory."
        echo "           Example: WHISPER_SDK_ROOT=/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk"
        exit 1
    fi

    if [[ ! -d "${WHISPER_SDK_ROOT}" ]]; then
        echo "[build.sh] ERROR: WHISPER_SDK_ROOT not found: ${WHISPER_SDK_ROOT}"
        exit 1
    fi

    echo "      Creating minimal whisper_sdk/ slice from: ${WHISPER_SDK_ROOT}"

    # Headers (for compilation): support SDK 2.6+ and 2.5 layouts.
    if [[ -d "${WHISPER_SDK_ROOT}/include/cpp" ]]; then
        mkdir -p "${WHISPER_SDK_DEST}/include/cpp"
        cp -r "${WHISPER_SDK_ROOT}/include/cpp/." \
              "${WHISPER_SDK_DEST}/include/cpp/"
    elif [[ -d "${WHISPER_SDK_ROOT}/include/npu/rpc/linux" ]]; then
        mkdir -p "${WHISPER_SDK_DEST}/include/npu/rpc/linux"
        cp -r "${WHISPER_SDK_ROOT}/include/npu/rpc/linux/." \
              "${WHISPER_SDK_DEST}/include/npu/rpc/linux/"
    else
        echo "[build.sh] ERROR: Whisper headers not found under ${WHISPER_SDK_ROOT}/include"
        echo "           Expected include/cpp (SDK 2.6+) or include/npu/rpc/linux (SDK 2.5)."
        exit 1
    fi

    # Linux runtime shared libraries
    LINUX_LIBS="${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/linux/whisper_all_quantized"
    if [[ ! -d "${LINUX_LIBS}" ]]; then
        echo "[build.sh] ERROR: Whisper linux libs not found: ${LINUX_LIBS}"
        exit 1
    fi
    mkdir -p "${WHISPER_SDK_DEST}/libs/npu/rpc_libraries/linux/whisper_all_quantized"
    cp "${LINUX_LIBS}/"*.so \
       "${WHISPER_SDK_DEST}/libs/npu/rpc_libraries/linux/whisper_all_quantized/"

    # VAD runtime assets (required for AIHub runtime directory use)
    ASSETS_DIR="${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/assets"
    if [[ -d "${ASSETS_DIR}" ]]; then
        mkdir -p "${WHISPER_SDK_DEST}/libs/npu/rpc_libraries/assets"
        cp -r "${ASSETS_DIR}/." \
           "${WHISPER_SDK_DEST}/libs/npu/rpc_libraries/assets/"
    else
        echo "[build.sh] WARNING: assets dir not found: ${ASSETS_DIR}"
        echo "           VAD assets will not be embedded in the image."
    fi

    echo "      whisper_sdk/ slice created:"
    du -sh "${WHISPER_SDK_DEST}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Optional: Prepare AIHub whisper_tiny runtime directory on host/device
# ─────────────────────────────────────────────────────────────────────────────
if [[ "${STT_PREPARE_MODEL}" == "1" ]]; then
    echo ""
    echo "[4.5/5] Preparing AIHub Whisper Tiny runtime at ${STT_MODEL_DIR} ..."
    LEGACY_STT_PREP="${SCRIPT_DIR}/model_generation_scripts/download_aihub_whisper_tiny.py"
    if [[ ! -f "${LEGACY_STT_PREP}" ]]; then
        echo "[build.sh] ERROR: Legacy model prep script not found: ${LEGACY_STT_PREP}"
        exit 1
    fi
    python3 "${LEGACY_STT_PREP}" \
        --out-dir "${STT_MODEL_DIR}" \
        --whisper-sdk-root "${WHISPER_SDK_ROOT:-${SCRIPT_DIR}/whisper_sdk}" \
        --download-precompiled
fi

# ─────────────────────────────────────────────────────────────────────────────
# Step 4: Build speech-to-text:latest
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "[5/5] Building speech-to-text:latest..."

cd "${SCRIPT_DIR}"
docker build ${NO_CACHE} \
    --progress=plain \
    -t speech-to-text:latest \
    .

echo ""
echo "============================================================"
echo " Build complete: speech-to-text:latest"
echo "============================================================"
docker images speech-to-text:latest
echo ""
echo " Run:"
echo "   docker run -d \\"
echo "     --name Speech2Text \\"
echo "     --restart always \\"
echo "     --network host \\"
echo "     -v /opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075:/opt/ASR_binary/Whisper/:ro \\"
echo "     -v /opt/qairt/current/qairt_245_flat_libs:/opt/qnn-host:ro \\"
echo "     --device qualcomm.com/device=cdi-hw-acc \\"
echo "     speech-to-text:latest"
echo ""
echo " Test:"
echo "   curl http://localhost:8081/health"
echo "============================================================"
