#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Validate reproducibility prerequisites before compose bring-up.
# ---------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

STRICT=0

usage() {
    cat <<USAGE
Usage: $0 [--strict]

Options:
  --strict   Fail when required host paths are missing
  --help     Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --strict)
            STRICT=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "Unknown argument: $1"
            ;;
    esac
done

cd "${REPO_ROOT}"

load_versions_manifest "${REPO_ROOT}"

require_cmd docker
require_cmd curl

if ! docker compose version >/dev/null 2>&1; then
    die "docker compose plugin is not available"
fi

compose_cfg="${REPO_ROOT}/test-evidence/compose_config_resolved.yaml"
mkdir -p "${REPO_ROOT}/test-evidence"
docker compose config > "${compose_cfg}"
log_info "Resolved compose config written to: ${compose_cfg}"

declare -a labels=(
    "HOST_RPC_LIB_DIR"
    "TG_MODEL_HOST_DIR"
    "TG_QAIRT_LIBS_HOST_DIR"
    "I2T_MODEL_HOST_DIR"
    "I2T_QAIRT_FLAT_LIB_DIR"
    "HF_CACHE_HOST_DIR"
    "IMAGEGEN_MODEL_DIR"
    "IMG_QAIRT_LIBS_HOST_DIR"
    "STT_MODEL_HOST_DIR"
    "STT_QNN_LIB_HOST_DIR"
    "TTS_MODEL_HOST_DIR"
)
declare -a values=(
    "${HOST_RPC_LIB_DIR:-/usr/lib}"
    "${TG_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-text}"
    "${TG_QAIRT_LIBS_HOST_DIR:-${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}}"
    "${I2T_MODEL_HOST_DIR:-/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075}"
    "${I2T_QAIRT_FLAT_LIB_DIR:-${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}}"
    "${HF_CACHE_HOST_DIR:-/opt/genai-studio-cache/huggingface}"
    "${IMAGEGEN_MODEL_DIR:-/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075}"
    "${IMG_QAIRT_LIBS_HOST_DIR:-${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}}"
    "${STT_MODEL_HOST_DIR:-/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075}"
    "${STT_QNN_LIB_HOST_DIR:-${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}}"
    "${TTS_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-speech/melo-tts-v73/files}"
)

missing=0
log_info "Checking resolved host paths used by docker-compose.yml"
for idx in "${!labels[@]}"; do
    label="${labels[$idx]}"
    value="${values[$idx]}"
    if [[ -e "${value}" ]]; then
        log_info "OK    ${label}=${value}"
    else
        log_warn "MISS  ${label}=${value}"
        missing=$((missing + 1))
    fi
done

tg_model_dir="${TG_MODEL_DIR:-/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075}"
tg_genie_config="${GENIE_CONFIG:-${tg_model_dir}/genie_config.json}"
if [[ -f "${tg_genie_config}" ]]; then
    log_info "Checking TG bundle references from ${tg_genie_config}"
    if ! python3 - "${tg_model_dir}" "${tg_genie_config}" <<'PY'
import json
import sys
from pathlib import Path

model_dir = Path(sys.argv[1])
cfg_path = Path(sys.argv[2])
cfg = json.loads(cfg_path.read_text())
refs = []

def walk(node):
    if isinstance(node, dict):
        for value in node.values():
            walk(value)
    elif isinstance(node, list):
        for value in node:
            walk(value)
    elif isinstance(node, str) and node.endswith((".bin", ".so", ".dat", ".json")):
        refs.append(node)

walk(cfg)
missing = []
for ref in sorted(set(refs)):
    if not (model_dir / ref).exists():
        missing.append(ref)

if missing:
    print("TG missing referenced files:")
    for ref in missing[:25]:
        print(f"  - {ref}")
    raise SystemExit(2)

print("TG bundle reference check OK")
PY
    then
        log_warn "MISS  TG model bundle has missing files referenced by genie_config.json"
        missing=$((missing + 1))
    fi
else
    log_warn "MISS  TG_GENIE_CONFIG=${tg_genie_config}"
    missing=$((missing + 1))
fi

i2t_model_dir="${I2T_MODEL_DIR:-/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075}"
if [[ -f "${i2t_model_dir}/libGenie.so" ]]; then
    log_info "OK    I2T artifact ${i2t_model_dir}/libGenie.so"
else
    log_warn "MISS  I2T artifact ${i2t_model_dir}/libGenie.so"
    missing=$((missing + 1))
fi

i2t_img_cfg=""
for cand in image_encoder.json img-enc-htp.json; do
    if [[ -f "${i2t_model_dir}/${cand}" ]]; then
        i2t_img_cfg="${cand}"
        break
    fi
done
if [[ -n "${i2t_img_cfg}" ]]; then
    log_info "OK    I2T image config ${i2t_model_dir}/${i2t_img_cfg}"
else
    log_warn "MISS  I2T image config (${i2t_model_dir}/image_encoder.json or img-enc-htp.json)"
    missing=$((missing + 1))
fi

i2t_text_cfg=""
for cand in text_generator.json text-dec-htp.json qwen2_5-vl-e2t-htp.json; do
    if [[ -f "${i2t_model_dir}/${cand}" ]]; then
        i2t_text_cfg="${cand}"
        break
    fi
done
if [[ -n "${i2t_text_cfg}" ]]; then
    log_info "OK    I2T text config ${i2t_model_dir}/${i2t_text_cfg}"
else
    log_warn "MISS  I2T text config (${i2t_model_dir}/text_generator.json or text-dec-htp.json or qwen2_5-vl-e2t-htp.json)"
    missing=$((missing + 1))
fi

tts_model_host_dir="${TTS_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-speech/melo-tts-v73/files}"
if [[ -d "${tts_model_host_dir}" ]]; then
    mapfile -t tts_qnns < <(find "${tts_model_host_dir}" -maxdepth 1 -type f -name "*.qnn*" | sort)
    if [[ "${#tts_qnns[@]}" -eq 0 ]]; then
        log_warn "MISS  no .qnn model found under TTS_MODEL_HOST_DIR=${tts_model_host_dir}"
        missing=$((missing + 1))
    else
        log_info "OK    TTS model files detected: ${#tts_qnns[@]}"
        has_v245=0
        has_v244=0
        for qnn in "${tts_qnns[@]}"; do
            case "${qnn}" in
                *v2.45.0.qnn*) has_v245=1 ;;
                *v2.44.0.qnn*) has_v244=1 ;;
            esac
        done
        if [[ "${has_v245}" -eq 1 && "${has_v244}" -eq 0 ]]; then
            log_warn "WARN  only v2.45 TTS model detected. If you hit tts_impl_init error=-2147482611, use a v2.44 model and set TTS_MODEL_HOST_DIR accordingly."
        fi
    fi
else
    log_warn "MISS  TTS_MODEL_HOST_DIR=${tts_model_host_dir}"
    missing=$((missing + 1))
fi

i2t_test_image="${REPO_ROOT}/assets/genai-studio-workflow.png"
if [[ -f "${i2t_test_image}" ]]; then
    log_info "OK    I2T sample image present: ${i2t_test_image}"
else
    log_warn "MISS  I2T sample image missing: ${i2t_test_image}"
    missing=$((missing + 1))
fi

declare -a required_device_nodes=(
    "/dev/fastrpc-cdsp"
    "/dev/fastrpc-cdsp-secure"
    "/dev/fastrpc-cdsp1"
    "/dev/fastrpc-cdsp1-secure"
    "/dev/fastrpc-adsp-secure"
)

log_info "Checking required FastRPC device nodes"
for dev in "${required_device_nodes[@]}"; do
    if [[ -c "${dev}" ]]; then
        log_info "OK    DEVICE=${dev}"
    else
        log_warn "MISS  DEVICE=${dev}"
        missing=$((missing + 1))
    fi
done

if [[ -z "${HOST_RPC_LIB_DIR:-}" ]]; then
    for d in /usr/lib/aarch64-linux-gnu /usr/lib; do
        if [[ -f "${d}/libcdsprpc.so.1" && -f "${d}/libdmabufheap.so.0" ]]; then
            HOST_RPC_LIB_DIR="${d}"
            break
        fi
    done
fi
HOST_RPC_LIB_DIR="${HOST_RPC_LIB_DIR:-/usr/lib}"

declare -a required_host_libs=(
    "${HOST_RPC_LIB_DIR}/libcdsprpc.so.1.0.0"
    "${HOST_RPC_LIB_DIR}/libcdsprpc.so.1"
    "${HOST_RPC_LIB_DIR}/libcdsprpc.so"
    "${HOST_RPC_LIB_DIR}/libdmabufheap.so.0"
)

log_info "Checking required host runtime libraries for bind mounts"
for lib in "${required_host_libs[@]}"; do
    if [[ -f "${lib}" || -L "${lib}" ]]; then
        log_info "OK    LIB=${lib}"
    elif [[ -d "${lib}" ]]; then
        log_warn "MISS  LIB=${lib} (is directory; expected file/symlink)"
        missing=$((missing + 1))
    else
        log_warn "MISS  LIB=${lib}"
        missing=$((missing + 1))
    fi
done

if [[ -f "${HOST_RPC_LIB_DIR}/libxdsprpc.so" ]]; then
    log_info "OK    LIB=${HOST_RPC_LIB_DIR}/libxdsprpc.so"
else
    log_info "INFO  optional host lib missing: ${HOST_RPC_LIB_DIR}/libxdsprpc.so"
fi

if [[ "${STRICT}" == "1" && "${missing}" -gt 0 ]]; then
    die "compose_doctor failed: ${missing} required path(s) missing"
fi

if [[ "${missing}" -gt 0 ]]; then
    log_warn "compose_doctor completed with warnings (${missing} missing path(s))"
else
    log_info "compose_doctor passed"
fi
