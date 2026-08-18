#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# Phase: Model Generation / Acquisition (T2T + I2T + STT + T2I + TTS source + TTS qnn pack)
# - Automates model download and preparation for:
#   * Text-to-Text (qwen3_4b)
#   * Image-to-Text (qwen2_5_vl_7b_instruct)
#   * Speech-to-Text (whisper-tiny)
#   * Text-to-Image (Stable Diffusion 2.1)
# - Validates required runtime artifacts based on service MODEL_SETUP docs.
# - For TTS, downloads AIHub source bins and packs final .qnn with wrapper script.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

DOWNLOAD_T2T=1
DOWNLOAD_I2T=1
DOWNLOAD_STT=1
DOWNLOAD_T2I=1
DOWNLOAD_TTS=1
VALIDATE_ONLY=0
GENERATE_TTS_QNN=1
VALIDATE_T2I=1
VALIDATE_STT=1
VALIDATE_TTS=1
SERVICE_FILTER_SET=0
FORCE_DOWNLOAD=0

T2T_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/qwen3_4b/releases/v0.57.2/qwen3_4b-genie-w4a16-qualcomm_qcs9075.zip"
I2T_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/qwen2_5_vl_7b_instruct/releases/v0.59.0/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075.zip"
STT_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/whisper_tiny/releases/v0.50.2/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip"
T2I_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/stable_diffusion_v2_1/releases/v0.50.2/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075.zip"
TTS_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/melotts_en/releases/v0.49.1/melotts_en-qnn_context_binary-mixed_with_float-qualcomm_qcs9075.zip"
T2I_TOKENIZER_VOCAB_URL="https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/vocab.json"
T2I_TOKENIZER_MERGES_URL="https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/merges.txt"

T2T_PARENT_DIR="/opt/genai-studio-models/text-to-text"
T2T_MODEL_DIR_DEFAULT="${T2T_PARENT_DIR}/qwen3_4b-genie-w4a16-qualcomm_qcs9075"
I2T_MODEL_DIR_DEFAULT="/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075"

STT_PARENT_DIR="/opt/genai-studio-models/speech-to-text"
STT_MODEL_DIR_DEFAULT="${STT_PARENT_DIR}/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075"

T2I_PARENT_DIR="/opt/genai-studio-models/text-to-image"
T2I_MODEL_DIR_DEFAULT="${T2I_PARENT_DIR}/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075"
TTS_PARENT_DIR="/opt/genai-studio-models/text-to-speech"
TTS_SOURCE_DIR_DEFAULT="${TTS_PARENT_DIR}/melotts_en-qnn_context_binary-mixed_with_float-qualcomm_qcs9075"
TTS_MODEL_DIR_DEFAULT="${TTS_PARENT_DIR}/melo-tts-v73/files"
# QNN wrapper scripts are kept in a separate model-conversion folder (not melo_sdk).
TTS_CONVERSION_ROOT_DEFAULT="${REPO_ROOT}/tools/model_conversion_scripts"
TTS_PACKER_SCRIPT_DEFAULT="${TTS_CONVERSION_ROOT_DEFAULT}/melo/py/qnn_model_generation.py"
# melo_sdk is used only to source libtts_impl_skel.so for runtime payload.
TTS_SDK_ROOT_DEFAULT="${REPO_ROOT}/core-services/text-to-speech/meloTTS/melo_sdk"
WHISPER_SDK_ROOT_DEFAULT="${REPO_ROOT}/core-services/speech-to-text/whisper_sdk"

usage() {
    cat <<USAGE
Usage: $0 [OPTIONS]

Automate T2T + I2T + STT + T2I + TTS-source model acquisition and validation.

Options:
  --service <name>      Run only selected service(s). Repeat flag or use comma list.
                        Names: text-to-text|t2t, image-to-text|i2t, speech-to-text|stt,
                               text-to-image|t2i, text-to-speech|tts
  --download-t2t        Download and prepare T2T model bundle (default: enabled)
  --skip-download-t2t   Skip T2T download/prep
  --download-i2t        Download and prepare I2T model bundle (default: enabled)
  --skip-download-i2t   Skip I2T download/prep
  --download-stt        Download and prepare STT model bundle (default: enabled)
  --skip-download-stt   Skip STT download/prep
  --download-t2i        Download and prepare T2I model bundle (default: enabled)
  --skip-download-t2i   Skip T2I download/prep
  --download-tts        Download TTS AIHub source bundle (default: enabled)
  --skip-download-tts   Skip TTS download/prep
  --generate-tts-qnn    Build packed TTS .qnn from source bins (default: enabled)
  --skip-generate-tts-qnn
                        Skip TTS qnn packing step
  --download-all        Enable T2T, I2T, STT, T2I, and TTS download/prep
  --download-none       Disable all downloads; validation still runs
  --validate-only       Alias for --download-none
  --force-download      Re-download/re-generate even if required artifacts are already present

  --t2t-url <src>       Override T2T bundle source (URL or local ZIP path)
  --i2t-url <src>       Override I2T bundle source (URL or local ZIP path)
  --stt-url <src>       Override STT bundle source (URL or local ZIP path)
  --t2i-url <src>       Override T2I bundle source (URL or local ZIP path)
  --tts-url <src>       Override TTS bundle source (URL or local ZIP path)

  --t2t-model-dir <dir> Override T2T target model dir
  --i2t-model-dir <dir> Override I2T target model dir
  --stt-model-dir <dir> Override STT target model dir
  --t2i-model-dir <dir> Override T2I target model dir
  --tts-source-dir <dir> Override TTS source-bundle dir (pre-pack)
  --tts-model-dir <dir> Override TTS runtime model dir (packed output)
  --tts-conversion-root <dir>
                        Root of model conversion scripts (default: repo/tools/model_conversion_scripts; separate from melo_sdk)
  --tts-packer-script <path>
                        Full path to qnn_model_generation.py
  --tts-sdk-root <dir>  Melo SDK root used only for libtts_impl_skel.so copy

  --help                Show this help

Environment overrides:
  TG_MODEL_DIR, I2T_MODEL_HOST_DIR, STT_MODEL_HOST_DIR, IMAGEGEN_MODEL_DIR, TTS_SOURCE_DIR, TTS_MODEL_HOST_DIR,
  TTS_CONVERSION_ROOT, TTS_PACKER_SCRIPT, TTS_SDK_ROOT, WHISPER_SDK_ROOT
USAGE
}

T2T_MODEL_DIR="${TG_MODEL_DIR:-${T2T_MODEL_DIR_DEFAULT}}"
I2T_MODEL_DIR="${I2T_MODEL_HOST_DIR:-${I2T_MODEL_DIR_DEFAULT}}"
STT_MODEL_DIR="${STT_MODEL_HOST_DIR:-${STT_MODEL_DIR_DEFAULT}}"
T2I_MODEL_DIR="${IMAGEGEN_MODEL_DIR:-${T2I_MODEL_DIR_DEFAULT}}"
TTS_SOURCE_DIR="${TTS_SOURCE_DIR:-${TTS_SOURCE_DIR_DEFAULT}}"
TTS_MODEL_DIR="${TTS_MODEL_HOST_DIR:-${TTS_MODEL_DIR_DEFAULT}}"
TTS_CONVERSION_ROOT="${TTS_CONVERSION_ROOT:-${TTS_CONVERSION_ROOT_DEFAULT}}"
TTS_PACKER_SCRIPT="${TTS_PACKER_SCRIPT:-${TTS_PACKER_SCRIPT_DEFAULT}}"
TTS_SDK_ROOT="${TTS_SDK_ROOT:-${TTS_SDK_ROOT_DEFAULT}}"
WHISPER_SDK_ROOT="${WHISPER_SDK_ROOT:-${WHISPER_SDK_ROOT_DEFAULT}}"

reset_all_service_toggles() {
    DOWNLOAD_T2T=0
    DOWNLOAD_I2T=0
    DOWNLOAD_STT=0
    DOWNLOAD_T2I=0
    DOWNLOAD_TTS=0
    GENERATE_TTS_QNN=0
    VALIDATE_T2I=0
    VALIDATE_STT=0
    VALIDATE_TTS=0
}

enable_service_toggle() {
    local service_name="${1,,}"
    case "${service_name}" in
        text-to-text|t2t)
            DOWNLOAD_T2T=1
            ;;
        image-to-text|i2t)
            DOWNLOAD_I2T=1
            ;;
        speech-to-text|stt)
            DOWNLOAD_STT=1
            VALIDATE_STT=1
            ;;
        text-to-image|t2i)
            DOWNLOAD_T2I=1
            VALIDATE_T2I=1
            ;;
        text-to-speech|tts)
            DOWNLOAD_TTS=1
            GENERATE_TTS_QNN=1
            VALIDATE_TTS=1
            ;;
        *)
            log_error "Unknown service for --service: ${service_name}"
            log_error "Supported: text-to-text|t2t, image-to-text|i2t, speech-to-text|stt, text-to-image|t2i, text-to-speech|tts"
            exit 1
            ;;
    esac
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --service)
            if [[ $# -lt 2 ]]; then
                log_error "--service requires an argument"
                exit 1
            fi
            if [[ ${SERVICE_FILTER_SET} -eq 0 ]]; then
                reset_all_service_toggles
                SERVICE_FILTER_SET=1
            fi
            IFS=',' read -r -a service_list <<< "$2"
            for service_item in "${service_list[@]}"; do
                service_item="${service_item// /}"
                [[ -z "${service_item}" ]] && continue
                enable_service_toggle "${service_item}"
            done
            shift 2
            ;;
        --download-t2t)
            DOWNLOAD_T2T=1
            shift
            ;;
        --skip-download-t2t)
            DOWNLOAD_T2T=0
            shift
            ;;
        --download-i2t)
            DOWNLOAD_I2T=1
            shift
            ;;
        --skip-download-i2t)
            DOWNLOAD_I2T=0
            shift
            ;;
        --download-stt)
            DOWNLOAD_STT=1
            shift
            ;;
        --skip-download-stt)
            DOWNLOAD_STT=0
            shift
            ;;
        --download-t2i)
            DOWNLOAD_T2I=1
            shift
            ;;
        --skip-download-t2i)
            DOWNLOAD_T2I=0
            shift
            ;;
        --download-tts)
            DOWNLOAD_TTS=1
            shift
            ;;
        --skip-download-tts)
            DOWNLOAD_TTS=0
            shift
            ;;
        --download-all)
            DOWNLOAD_T2T=1
            DOWNLOAD_I2T=1
            DOWNLOAD_STT=1
            DOWNLOAD_T2I=1
            DOWNLOAD_TTS=1
            GENERATE_TTS_QNN=1
            VALIDATE_T2I=1
            VALIDATE_STT=1
            VALIDATE_TTS=1
            shift
            ;;
        --download-none)
            DOWNLOAD_T2T=0
            DOWNLOAD_I2T=0
            DOWNLOAD_STT=0
            DOWNLOAD_T2I=0
            DOWNLOAD_TTS=0
            GENERATE_TTS_QNN=0
            VALIDATE_T2I=0
            VALIDATE_STT=0
            VALIDATE_TTS=0
            shift
            ;;
        --validate-only)
            DOWNLOAD_T2T=0
            DOWNLOAD_I2T=0
            DOWNLOAD_STT=0
            DOWNLOAD_T2I=0
            DOWNLOAD_TTS=0
            GENERATE_TTS_QNN=0
            VALIDATE_T2I=1
            VALIDATE_STT=1
            VALIDATE_TTS=1
            VALIDATE_ONLY=1
            shift
            ;;
        --force-download)
            FORCE_DOWNLOAD=1
            shift
            ;;
        --generate-tts-qnn)
            GENERATE_TTS_QNN=1
            shift
            ;;
        --skip-generate-tts-qnn)
            GENERATE_TTS_QNN=0
            shift
            ;;
        --t2t-url)
            T2T_ZIP_URL="$2"
            shift 2
            ;;
        --i2t-url)
            I2T_ZIP_URL="$2"
            shift 2
            ;;
        --stt-url)
            STT_ZIP_URL="$2"
            shift 2
            ;;
        --t2i-url)
            T2I_ZIP_URL="$2"
            shift 2
            ;;
        --tts-url)
            TTS_ZIP_URL="$2"
            shift 2
            ;;
        --t2t-model-dir)
            T2T_MODEL_DIR="$2"
            shift 2
            ;;
        --i2t-model-dir)
            I2T_MODEL_DIR="$2"
            shift 2
            ;;
        --stt-model-dir)
            STT_MODEL_DIR="$2"
            shift 2
            ;;
        --t2i-model-dir)
            T2I_MODEL_DIR="$2"
            shift 2
            ;;
        --tts-source-dir)
            TTS_SOURCE_DIR="$2"
            shift 2
            ;;
        --tts-model-dir)
            TTS_MODEL_DIR="$2"
            shift 2
            ;;
        --tts-conversion-root)
            TTS_CONVERSION_ROOT="$2"
            TTS_PACKER_SCRIPT="${TTS_CONVERSION_ROOT}/melo/py/qnn_model_generation.py"
            shift 2
            ;;
        --tts-packer-script)
            TTS_PACKER_SCRIPT="$2"
            shift 2
            ;;
        --tts-sdk-root)
            TTS_SDK_ROOT="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown argument: $1"
            usage
            exit 1
            ;;
    esac
done

require_cmd python3

log_info "==================================================================="
log_info "Model Generation / Acquisition: T2T + I2T + STT + T2I + TTS source"
log_info "==================================================================="
log_info "T2T target dir : ${T2T_MODEL_DIR}"
log_info "I2T target dir : ${I2T_MODEL_DIR}"
log_info "STT target dir : ${STT_MODEL_DIR}"
log_info "T2I target dir : ${T2I_MODEL_DIR}"
log_info "TTS source dir : ${TTS_SOURCE_DIR}"
log_info "TTS model dir  : ${TTS_MODEL_DIR}"
log_info "TTS packer     : ${TTS_PACKER_SCRIPT}"
if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
    log_info "Mode          : validate-only"
fi

FAILURES=0

parent_of() {
    local p="$1"
    dirname "$p"
}

resolve_bundle_to_tmp() {
    local bundle_source="$1"
    local tmp_zip="$2"

    if [[ -f "${bundle_source}" ]]; then
        log_info "  SRC : local file ${bundle_source}"
        cp -f "${bundle_source}" "${tmp_zip}"
        return 0
    fi

    log_info "  SRC : remote URL ${bundle_source}"
    require_cmd wget
    wget -O "${tmp_zip}" "${bundle_source}"
}

download_zip_model_bundle() {
    local service_name="$1"
    local bundle_source="$2"
    local expected_dir="$3"

    require_cmd unzip

    local dest_parent
    dest_parent="$(parent_of "${expected_dir}")"

    mkdir -p "${dest_parent}"

    local zip_name
    zip_name="$(basename "${bundle_source}")"
    local tmp_zip="/tmp/${zip_name}"

    log_info ""
    log_info "Downloading ${service_name} bundle"
    log_info "  ZIP : ${tmp_zip}"
    log_info "  DST : ${dest_parent}"

    resolve_bundle_to_tmp "${bundle_source}" "${tmp_zip}"
    unzip -o "${tmp_zip}" -d "${dest_parent}"

    if [[ -d "${expected_dir}" ]]; then
        log_info "  [PASS] ${service_name} bundle ready: ${expected_dir}"
    else
        log_error "  [FAIL] ${service_name} expected directory missing after unzip: ${expected_dir}"
        FAILURES=$((FAILURES + 1))
    fi
}

download_t2t_bundle() {
    local bundle_source="$1"
    local expected_dir="$2"

    local dest_parent
    dest_parent="$(parent_of "${expected_dir}")"
    mkdir -p "${dest_parent}"

    local zip_name
    zip_name="$(basename "${bundle_source}")"
    local tmp_zip="/tmp/${zip_name}"

    log_info ""
    log_info "Downloading Text-to-Text bundle"
    log_info "  ZIP : ${tmp_zip}"
    log_info "  DST : ${dest_parent}"

    resolve_bundle_to_tmp "${bundle_source}" "${tmp_zip}"

    local extracted=0
    if command -v 7z >/dev/null 2>&1; then
        log_info "Extracting T2T with: 7z x"
        if 7z x -y -o"${dest_parent}" "${tmp_zip}" >/dev/null; then
            extracted=1
        fi
    fi

    if [[ ${extracted} -ne 1 ]]; then
        require_cmd unzip
        log_warn "Falling back to unzip for T2T extraction"
        unzip -o "${tmp_zip}" -d "${dest_parent}" >/dev/null
    fi

    if [[ -d "${expected_dir}" ]]; then
        log_info "  [PASS] Text-to-Text bundle ready: ${expected_dir}"
    else
        log_error "  [FAIL] Text-to-Text expected directory missing after extraction: ${expected_dir}"
        FAILURES=$((FAILURES + 1))
    fi
}

download_i2t_bundle() {
    local bundle_source="$1"
    local expected_dir="$2"
    local qairt_flat_lib_dir="${I2T_QAIRT_FLAT_LIB_DIR:-${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}}"

    require_cmd wget

    local dest_parent
    dest_parent="$(parent_of "${expected_dir}")"
    mkdir -p "${dest_parent}"

    local zip_name
    zip_name="$(basename "${bundle_source}")"
    local tmp_zip="/tmp/${zip_name}"

    log_info ""
    log_info "Downloading Image-to-Text bundle"
    log_info "  ZIP : ${tmp_zip}"
    log_info "  DST : ${dest_parent}"

    resolve_bundle_to_tmp "${bundle_source}" "${tmp_zip}"

    local extracted=0
    if command -v 7z >/dev/null 2>&1; then
        log_info "Extracting I2T with: 7z x"
        if 7z x -y -o"${dest_parent}" "${tmp_zip}" >/dev/null; then
            extracted=1
        fi
    else
        log_info "7z not found; trying install for preferred extraction path"
        if command -v apt-get >/dev/null 2>&1; then
            if command -v sudo >/dev/null 2>&1; then
                sudo apt-get update >/dev/null 2>&1 || true
                sudo apt-get install -y p7zip-full >/dev/null 2>&1 || true
            else
                apt-get update >/dev/null 2>&1 || true
                apt-get install -y p7zip-full >/dev/null 2>&1 || true
            fi
        fi
        if command -v 7z >/dev/null 2>&1; then
            log_info "Extracting I2T with: 7z x (post-install)"
            if 7z x -y -o"${dest_parent}" "${tmp_zip}" >/dev/null; then
                extracted=1
            fi
        fi
    fi

    if [[ ${extracted} -ne 1 ]]; then
        require_cmd unzip
        log_warn "Falling back to unzip for I2T extraction"
        unzip -o "${tmp_zip}" -d "${dest_parent}" >/dev/null
    fi

    if [[ -d "${expected_dir}" ]]; then
        log_info "  [PASS] Image-to-Text bundle ready: ${expected_dir}"
    else
        log_error "  [FAIL] Image-to-Text expected directory missing after extraction: ${expected_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if [[ ! -f "${expected_dir}/libGenie.so" ]]; then
        if [[ -f "${qairt_flat_lib_dir}/libGenie.so" ]]; then
            cp -f "${qairt_flat_lib_dir}/libGenie.so" "${expected_dir}/libGenie.so"
            log_info "  [PASS] Copied libGenie.so from QAIRT flat libs: ${qairt_flat_lib_dir}"
        else
            log_warn "  [WARN] libGenie.so not found in QAIRT flat libs: ${qairt_flat_lib_dir}"
        fi
    fi

    mkdir -p "${expected_dir}/uploads"
    log_info "  [PASS] Ensured uploads/ exists: ${expected_dir}/uploads"
}

download_t2i_tokenizer_if_missing() {
    local model_dir="$1"
    local tokenizer_dir="${model_dir}/tokenizer"

    mkdir -p "${tokenizer_dir}"

    if [[ ! -f "${tokenizer_dir}/vocab.json" ]]; then
        log_info "Downloading T2I tokenizer vocab.json"
        wget -O "${tokenizer_dir}/vocab.json" "${T2I_TOKENIZER_VOCAB_URL}"
    fi

    if [[ ! -f "${tokenizer_dir}/merges.txt" ]]; then
        log_info "Downloading T2I tokenizer merges.txt"
        wget -O "${tokenizer_dir}/merges.txt" "${T2I_TOKENIZER_MERGES_URL}"
    fi
}

copy_stt_vad_if_available() {
    local stt_model_dir="$1"
    local vad_dst="${stt_model_dir}/libnnvad_model.so"

    if [[ -f "${vad_dst}" ]]; then
        log_info "STT VAD asset already present: ${vad_dst}"
        return 0
    fi

    local candidates=(
        "${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so"
        "/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so"
        "/opt/qcom/VoiceAI_ASR/2.6.0.0/whisper_sdk/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so"
    )

    local src=""
    for cand in "${candidates[@]}"; do
        if [[ -f "${cand}" ]]; then
            src="${cand}"
            break
        fi
    done

    if [[ -n "${src}" ]]; then
        cp -f "${src}" "${vad_dst}"
        log_info "Copied STT VAD asset from: ${src}"
    else
        log_warn "STT VAD asset not found in known SDK paths."
        log_warn "Expected one of:"
        for cand in "${candidates[@]}"; do
            log_warn "  - ${cand}"
        done
    fi
}

validate_t2i() {
    local model_dir="$1"

    log_info ""
    log_info "Validating Text-to-Image model artifacts"

    if [[ ! -d "${model_dir}" ]]; then
        log_error "  [FAIL] T2I model directory NOT FOUND: ${model_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    log_info "  [PASS] T2I model directory exists: ${model_dir}"

    local text_enc_bins
    local unet_bins
    local vae_bins
    text_enc_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*text_encoder*.bin" 2>/dev/null | wc -l)
    unet_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*unet*.bin" 2>/dev/null | wc -l)
    vae_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*vae*.bin" 2>/dev/null | wc -l)

    if [[ ${text_enc_bins} -gt 0 ]] && [[ ${unet_bins} -gt 0 ]] && [[ ${vae_bins} -gt 0 ]]; then
        log_info "  [PASS] Found required bins: text_encoder, unet, vae"
    else
        log_error "  [FAIL] Missing required T2I bins"
        [[ ${text_enc_bins} -eq 0 ]] && log_error "         Missing: *text_encoder*.bin"
        [[ ${unet_bins} -eq 0 ]] && log_error "         Missing: *unet*.bin"
        [[ ${vae_bins} -eq 0 ]] && log_error "         Missing: *vae*.bin"
        FAILURES=$((FAILURES + 1))
    fi

    if [[ -f "${model_dir}/tokenizer/vocab.json" ]]; then
        log_info "  [PASS] tokenizer/vocab.json found"
    else
        log_error "  [FAIL] tokenizer/vocab.json missing"
        FAILURES=$((FAILURES + 1))
    fi

    if [[ -f "${model_dir}/tokenizer/merges.txt" ]]; then
        log_info "  [PASS] tokenizer/merges.txt found"
    else
        log_error "  [FAIL] tokenizer/merges.txt missing"
        FAILURES=$((FAILURES + 1))
    fi
}

validate_stt() {
    local model_dir="$1"

    log_info ""
    log_info "Validating Speech-to-Text model artifacts"

    if [[ ! -d "${model_dir}" ]]; then
        log_error "  [FAIL] STT model directory NOT FOUND: ${model_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    log_info "  [PASS] STT model directory exists: ${model_dir}"

    local encoder_bins
    local decoder_bins
    local vocab_bins
    encoder_bins=$(find "${model_dir}" -maxdepth 2 -type f \( -name "encoder.bin" -o -name "*encoder*.bin" \) 2>/dev/null | wc -l)
    decoder_bins=$(find "${model_dir}" -maxdepth 2 -type f \( -name "decoder.bin" -o -name "*decoder*.bin" \) 2>/dev/null | wc -l)
    vocab_bins=$(find "${model_dir}" -maxdepth 2 -type f -name "vocab.bin" 2>/dev/null | wc -l)

    if [[ ${encoder_bins} -gt 0 ]] && [[ ${decoder_bins} -gt 0 ]] && [[ ${vocab_bins} -gt 0 ]]; then
        log_info "  [PASS] Found required STT bins: encoder, decoder, vocab"
    else
        log_error "  [FAIL] Missing required STT bins"
        [[ ${encoder_bins} -eq 0 ]] && log_error "         Missing: encoder.bin or compatible encoder*.bin"
        [[ ${decoder_bins} -eq 0 ]] && log_error "         Missing: decoder.bin or compatible decoder*.bin"
        [[ ${vocab_bins} -eq 0 ]] && log_error "         Missing: vocab.bin"
        FAILURES=$((FAILURES + 1))
    fi

    local vad_found=0
    for vad_path in "${model_dir}/libnnvad_model.so" "/opt/asr-assets/libnnvad_model.so"; do
        if [[ -f "${vad_path}" ]]; then
            log_info "  [PASS] VAD model found: ${vad_path}"
            vad_found=1
            break
        fi
    done

    if [[ ${vad_found} -eq 0 ]]; then
        log_warn "  [WARN] VAD model not found (libnnvad_model.so)"
        log_warn "         Expected in ${model_dir}/ or /opt/asr-assets/"
        log_warn "         Service may fail without VAD model"
    fi
}

validate_tts_source_bundle() {
    local source_dir="$1"

    log_info ""
    log_info "Validating TTS source bundle artifacts (pre-pack)"

    if [[ ! -d "${source_dir}" ]]; then
        log_error "  [FAIL] TTS source directory NOT FOUND: ${source_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    log_info "  [PASS] TTS source directory exists: ${source_dir}"

    local required=(
        "config.json"
        "bert_en_tokenizer.bin"
        "bert_normalizer.bin"
    )
    local optional_aliases=(
        "BertWrapper_EN.bin|bert_wrapper.bin"
        "Encoder_EN.bin|encoder.bin"
        "Flow_EN.bin|flow.bin"
        "Decoder_EN.bin|decoder.bin"
        "T5Encoder_EN.bin|t5_encoder.bin"
        "T5Decoder_EN.bin|t5_decoder.bin"
    )

    local f=""
    for f in "${required[@]}"; do
        if [[ -f "${source_dir}/${f}" ]]; then
            log_info "  [PASS] Found ${f}"
        else
            log_error "  [FAIL] Missing ${f}"
            FAILURES=$((FAILURES + 1))
        fi
    done

    local pair=""
    for pair in "${optional_aliases[@]}"; do
        IFS='|' read -r a b <<< "${pair}"
        if [[ -f "${source_dir}/${a}" || -f "${source_dir}/${b}" ]]; then
            log_info "  [PASS] Found ${a} or ${b}"
        else
            log_error "  [FAIL] Missing both ${a} and ${b}"
            FAILURES=$((FAILURES + 1))
        fi
    done
}

has_t2t_artifacts() {
    local model_dir="$1"
    [[ -d "${model_dir}" && -f "${model_dir}/genie_config.json" ]]
}

has_i2t_artifacts() {
    local model_dir="$1"
    local img_cfg=""
    local txt_cfg=""
    local dec_cfg=""

    for img_cfg in "img-enc-htp.json" "image_encoder.json"; do
        if [[ -f "${model_dir}/${img_cfg}" ]]; then
            break
        fi
        img_cfg=""
    done
    for txt_cfg in "text-encoder.json" "text_encoder.json"; do
        if [[ -f "${model_dir}/${txt_cfg}" ]]; then
            break
        fi
        txt_cfg=""
    done
    for dec_cfg in "text-dec-htp.json" "text-generator.json"; do
        if [[ -f "${model_dir}/${dec_cfg}" ]]; then
            break
        fi
        dec_cfg=""
    done

    [[ -d "${model_dir}" && -n "${img_cfg}" && -n "${txt_cfg}" && -n "${dec_cfg}" && -f "${model_dir}/libGenie.so" && -d "${model_dir}/uploads" ]]
}

has_t2i_artifacts() {
    local model_dir="$1"
    local text_enc_bins
    local unet_bins
    local vae_bins

    [[ -d "${model_dir}" ]] || return 1
    text_enc_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*text_encoder*.bin" 2>/dev/null | wc -l)
    unet_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*unet*.bin" 2>/dev/null | wc -l)
    vae_bins=$(find "${model_dir}" -maxdepth 1 -type f -name "*vae*.bin" 2>/dev/null | wc -l)

    [[ ${text_enc_bins} -gt 0 && ${unet_bins} -gt 0 && ${vae_bins} -gt 0 && -f "${model_dir}/tokenizer/vocab.json" && -f "${model_dir}/tokenizer/merges.txt" ]]
}

has_stt_artifacts() {
    local model_dir="$1"
    local encoder_bins
    local decoder_bins
    local vocab_bins

    [[ -d "${model_dir}" ]] || return 1
    encoder_bins=$(find "${model_dir}" -maxdepth 2 -type f \( -name "encoder.bin" -o -name "*encoder*.bin" \) 2>/dev/null | wc -l)
    decoder_bins=$(find "${model_dir}" -maxdepth 2 -type f \( -name "decoder.bin" -o -name "*decoder*.bin" \) 2>/dev/null | wc -l)
    vocab_bins=$(find "${model_dir}" -maxdepth 2 -type f -name "vocab.bin" 2>/dev/null | wc -l)

    [[ ${encoder_bins} -gt 0 && ${decoder_bins} -gt 0 && ${vocab_bins} -gt 0 ]]
}

has_tts_source_artifacts() {
    local source_dir="$1"
    [[ -d "${source_dir}" && \
       -f "${source_dir}/config.json" && \
       -f "${source_dir}/bert_en_tokenizer.bin" && \
       -f "${source_dir}/bert_normalizer.bin" ]]
}

has_tts_runtime_artifacts() {
    local out_dir="$1"
    local qnn_count=0
    [[ -d "${out_dir}" ]] || return 1
    qnn_count=$(find "${out_dir}" -maxdepth 1 -type f -name "*.qnn" 2>/dev/null | wc -l)
    [[ ${qnn_count} -gt 0 && -f "${out_dir}/libtts_impl_skel.so" ]]
}

find_first_existing() {
    local base_dir="$1"
    shift
    local rel=""
    for rel in "$@"; do
        if [[ -f "${base_dir}/${rel}" ]]; then
            printf '%s' "${base_dir}/${rel}"
            return 0
        fi
    done
    return 1
}

copy_tts_runtime_support_libs() {
    local dst_dir="$1"
    mkdir -p "${dst_dir}"

    local skel_candidates=(
        "${TTS_SDK_ROOT}/libs/npu/rpc_libraries/cdsp/libtts_impl_skel.so"
        "${REPO_ROOT}/core-services/text-to-speech/meloTTS/melo_sdk/libs/npu/rpc_libraries/cdsp/libtts_impl_skel.so"
        "/opt/qcom/VoiceAI_TTS/1.1.1.0/melo_sdk/libs/npu/rpc_libraries/cdsp/libtts_impl_skel.so"
        "/local/mnt/workspace/qpm/VoiceAI_TTS/1.1.1.0/melo_sdk/libs/npu/rpc_libraries/cdsp/libtts_impl_skel.so"
    )

    local src=""
    local cand=""
    for cand in "${skel_candidates[@]}"; do
        if [[ -f "${cand}" ]]; then
            src="${cand}"
            break
        fi
    done
    if [[ -n "${src}" ]]; then
        cp -f "${src}" "${dst_dir}/libtts_impl_skel.so"
        log_info "  [PASS] Copied libtts_impl_skel.so from: ${src}"
    else
        log_warn "  [WARN] libtts_impl_skel.so not found in known SDK paths"
    fi
}

generate_tts_qnn_bundle() {
    local source_dir="$1"
    local out_dir="$2"
    local effective_packer="${TTS_PACKER_SCRIPT}"
    local compat_packer=""
    local packer_module_root=""
    cleanup_compat_packer() {
        if [[ -n "${compat_packer}" ]]; then
            rm -f "${compat_packer}" || true
            compat_packer=""
        fi
    }

    log_info ""
    log_info "Generating TTS packed qnn bundle"

    if [[ ! -d "${source_dir}" ]]; then
        log_error "  [FAIL] TTS source directory missing: ${source_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if [[ ! -f "${TTS_PACKER_SCRIPT}" ]]; then
        log_error "  [FAIL] TTS packer script missing: ${TTS_PACKER_SCRIPT}"
        FAILURES=$((FAILURES + 1))
        return
    fi
    packer_module_root="$(dirname "${TTS_PACKER_SCRIPT}")"
    if [[ ! -d "${packer_module_root}" ]]; then
        log_error "  [FAIL] TTS packer module root missing: ${packer_module_root}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if ! grep -q "scratch_mem_size_req" "${TTS_PACKER_SCRIPT}"; then
        log_error "  [FAIL] TTS packer does not support --scratch_mem_size_req: ${TTS_PACKER_SCRIPT}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if grep -q "libQnnHtpV81.so" "${TTS_PACKER_SCRIPT}"; then
        compat_packer="$(mktemp /tmp/qnn_model_generation_v73_XXXXXX.py)"
        if sed 's/libQnnHtpV81\.so/libQnnHtpV73.so/g' "${TTS_PACKER_SCRIPT}" > "${compat_packer}"; then
            chmod +x "${compat_packer}" || true
            effective_packer="${compat_packer}"
            log_warn "  [WARN] Packer references libQnnHtpV81.so; using V73 compatibility shim: ${compat_packer}"
        else
            rm -f "${compat_packer}" || true
            compat_packer=""
            log_warn "  [WARN] Failed to prepare V73 packer shim; continuing with original packer"
        fi
    fi

    local cfg="${source_dir}/config.json"
    if [[ ! -f "${cfg}" ]]; then
        cleanup_compat_packer
        log_error "  [FAIL] Missing TTS config.json: ${cfg}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    local parsed
    parsed="$(python3 - "${cfg}" <<'PY'
import json,sys
cfg = json.load(open(sys.argv[1], 'r', encoding='utf-8'))
rt = cfg.get('runtime', {})
assets = cfg.get('assets', {})
qv = rt.get('qnn_version', {})
qmaj = rt.get('qnn_version_major', qv.get('major', 2))
qmin = rt.get('qnn_version_minor', qv.get('minor', 43))
qpch = rt.get('qnn_version_patch', qv.get('patch', 0))
lang = rt.get('language', 'en')
scratch = rt.get('scratch_mem_size_req', 3200000)
for k,v in [
  ('QNN_MAJ', qmaj), ('QNN_MIN', qmin), ('QNN_PCH', qpch),
  ('MODEL_LANG', lang), ('SCRATCH_MEM', scratch),
  ('BERT_MODEL', assets.get('bert_model','')),
  ('BERT_TOKENIZER', assets.get('bert_tokenizer','')),
  ('BERT_NORMALIZER', assets.get('bert_normalizer','')),
  ('MELO_ENCODER', assets.get('melo_encoder','')),
  ('MELO_FLOW', assets.get('melo_flow','')),
  ('MELO_DECODER', assets.get('melo_decoder','')),
  ('G2P_ENCODER', assets.get('g2p_encoder','')),
  ('G2P_DECODER', assets.get('g2p_decoder','')),
]:
    print(f"{k}={v}")
PY
)"

    local QNN_MAJ="" QNN_MIN="" QNN_PCH="" MODEL_LANG="" SCRATCH_MEM=""
    local BERT_MODEL="" BERT_TOKENIZER="" BERT_NORMALIZER=""
    local MELO_ENCODER="" MELO_FLOW="" MELO_DECODER="" G2P_ENCODER="" G2P_DECODER=""
    while IFS='=' read -r key val; do
        case "${key}" in
            QNN_MAJ) QNN_MAJ="${val}" ;;
            QNN_MIN) QNN_MIN="${val}" ;;
            QNN_PCH) QNN_PCH="${val}" ;;
            MODEL_LANG) MODEL_LANG="${val}" ;;
            SCRATCH_MEM) SCRATCH_MEM="${val}" ;;
            BERT_MODEL) BERT_MODEL="${val}" ;;
            BERT_TOKENIZER) BERT_TOKENIZER="${val}" ;;
            BERT_NORMALIZER) BERT_NORMALIZER="${val}" ;;
            MELO_ENCODER) MELO_ENCODER="${val}" ;;
            MELO_FLOW) MELO_FLOW="${val}" ;;
            MELO_DECODER) MELO_DECODER="${val}" ;;
            G2P_ENCODER) G2P_ENCODER="${val}" ;;
            G2P_DECODER) G2P_DECODER="${val}" ;;
        esac
    done <<< "${parsed}"

    local bert_model_path=""
    local bert_tokenizer_path=""
    local bert_normalizer_path=""
    local melo_encoder_path=""
    local melo_flow_path=""
    local melo_decoder_path=""
    local g2p_encoder_path=""
    local g2p_decoder_path=""

    bert_model_path="$(find_first_existing "${source_dir}" "${BERT_MODEL}" "BertWrapper_EN.bin" "bert_wrapper.bin")" || true
    bert_tokenizer_path="$(find_first_existing "${source_dir}" "${BERT_TOKENIZER}" "bert_en_tokenizer.bin")" || true
    bert_normalizer_path="$(find_first_existing "${source_dir}" "${BERT_NORMALIZER}" "bert_normalizer.bin")" || true
    melo_encoder_path="$(find_first_existing "${source_dir}" "${MELO_ENCODER}" "Encoder_EN.bin" "encoder.bin")" || true
    melo_flow_path="$(find_first_existing "${source_dir}" "${MELO_FLOW}" "Flow_EN.bin" "flow.bin")" || true
    melo_decoder_path="$(find_first_existing "${source_dir}" "${MELO_DECODER}" "Decoder_EN.bin" "decoder.bin")" || true
    g2p_encoder_path="$(find_first_existing "${source_dir}" "${G2P_ENCODER}" "T5Encoder_EN.bin" "t5_encoder.bin")" || true
    g2p_decoder_path="$(find_first_existing "${source_dir}" "${G2P_DECODER}" "T5Decoder_EN.bin" "t5_decoder.bin")" || true

    local missing=0
    local p=""
    for p in \
        "${bert_model_path}" "${bert_tokenizer_path}" "${bert_normalizer_path}" \
        "${melo_encoder_path}" "${melo_flow_path}" "${melo_decoder_path}" \
        "${g2p_encoder_path}" "${g2p_decoder_path}"; do
        if [[ -z "${p}" ]]; then
            missing=1
        fi
    done
    if [[ ${missing} -ne 0 ]]; then
        cleanup_compat_packer
        log_error "  [FAIL] Could not resolve all required TTS source bins from ${source_dir}"
        FAILURES=$((FAILURES + 1))
        return
    fi

    mkdir -p "${out_dir}"
    local out_qnn="${out_dir}/melo_${MODEL_LANG}.64_bit.qnn_v${QNN_MAJ}.${QNN_MIN}.${QNN_PCH}.qnn"

    log_info "  Packer: ${effective_packer}"
    log_info "  Output: ${out_qnn}"
    if ! (
        cd "${packer_module_root}"
        PYTHONPATH="${packer_module_root}:${PYTHONPATH:-}" \
        python3 "${effective_packer}" \
            --bert_model "${bert_model_path}" \
            --bert_tokenizer "${bert_tokenizer_path}" \
            --bert_normalizer "${bert_normalizer_path}" \
            --melo_encoder_model "${melo_encoder_path}" \
            --melo_flow_model "${melo_flow_path}" \
            --melo_decoder_model "${melo_decoder_path}" \
            --g2p_enc_model "${g2p_encoder_path}" \
            --g2p_dec_model "${g2p_decoder_path}" \
            --qnn_version_major "${QNN_MAJ}" \
            --qnn_version_minor "${QNN_MIN}" \
            --qnn_version_patch "${QNN_PCH}" \
            --arch 64 \
            --model_lang "${MODEL_LANG}" \
            --scratch_mem_size_req "${SCRATCH_MEM}" \
            --path_out_model "${out_qnn}"
    ); then
        cleanup_compat_packer
        log_error "  [FAIL] TTS qnn generation command failed"
        FAILURES=$((FAILURES + 1))
        return
    fi

    if [[ -f "${out_qnn}" ]]; then
        log_info "  [PASS] Generated packed TTS qnn: ${out_qnn}"
    else
        log_error "  [FAIL] TTS qnn generation did not produce expected output: ${out_qnn}"
        FAILURES=$((FAILURES + 1))
        cleanup_compat_packer
        return
    fi

    copy_tts_runtime_support_libs "${out_dir}"
    cleanup_compat_packer
}

if [[ ${DOWNLOAD_T2T} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Text-to-Text; refreshing bundle: ${T2T_MODEL_DIR}"
        download_t2t_bundle "${T2T_ZIP_URL}" "${T2T_MODEL_DIR}"
    elif has_t2t_artifacts "${T2T_MODEL_DIR}"; then
        log_info "Skipping Text-to-Text download; required artifacts already present: ${T2T_MODEL_DIR}"
    else
        download_t2t_bundle "${T2T_ZIP_URL}" "${T2T_MODEL_DIR}"
    fi
fi

if [[ ${DOWNLOAD_I2T} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Image-to-Text; refreshing bundle: ${I2T_MODEL_DIR}"
        download_i2t_bundle "${I2T_ZIP_URL}" "${I2T_MODEL_DIR}"
    elif has_i2t_artifacts "${I2T_MODEL_DIR}"; then
        log_info "Skipping Image-to-Text download; required artifacts already present: ${I2T_MODEL_DIR}"
    else
        download_i2t_bundle "${I2T_ZIP_URL}" "${I2T_MODEL_DIR}"
    fi
fi

if [[ ${DOWNLOAD_T2I} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Text-to-Image; refreshing bundle: ${T2I_MODEL_DIR}"
        download_zip_model_bundle "Text-to-Image" "${T2I_ZIP_URL}" "${T2I_MODEL_DIR}"
        if [[ -d "${T2I_MODEL_DIR}" ]]; then
            download_t2i_tokenizer_if_missing "${T2I_MODEL_DIR}"
        fi
    elif has_t2i_artifacts "${T2I_MODEL_DIR}"; then
        log_info "Skipping Text-to-Image download; required artifacts already present: ${T2I_MODEL_DIR}"
    else
        download_zip_model_bundle "Text-to-Image" "${T2I_ZIP_URL}" "${T2I_MODEL_DIR}"
        if [[ -d "${T2I_MODEL_DIR}" ]]; then
            download_t2i_tokenizer_if_missing "${T2I_MODEL_DIR}"
        fi
    fi
fi

if [[ ${DOWNLOAD_STT} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Speech-to-Text; refreshing bundle: ${STT_MODEL_DIR}"
        download_zip_model_bundle "Speech-to-Text" "${STT_ZIP_URL}" "${STT_MODEL_DIR}"
        if [[ -d "${STT_MODEL_DIR}" ]]; then
            copy_stt_vad_if_available "${STT_MODEL_DIR}"
        fi
    elif has_stt_artifacts "${STT_MODEL_DIR}"; then
        log_info "Skipping Speech-to-Text download; required artifacts already present: ${STT_MODEL_DIR}"
    else
        download_zip_model_bundle "Speech-to-Text" "${STT_ZIP_URL}" "${STT_MODEL_DIR}"
        if [[ -d "${STT_MODEL_DIR}" ]]; then
            copy_stt_vad_if_available "${STT_MODEL_DIR}"
        fi
    fi
    if [[ -d "${STT_MODEL_DIR}" ]]; then
        copy_stt_vad_if_available "${STT_MODEL_DIR}"
    fi
fi

if [[ ${DOWNLOAD_TTS} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Text-to-Speech source; refreshing bundle: ${TTS_SOURCE_DIR}"
        download_zip_model_bundle "Text-to-Speech source" "${TTS_ZIP_URL}" "${TTS_SOURCE_DIR}"
    elif has_tts_source_artifacts "${TTS_SOURCE_DIR}"; then
        log_info "Skipping Text-to-Speech source download; required artifacts already present: ${TTS_SOURCE_DIR}"
    else
        download_zip_model_bundle "Text-to-Speech source" "${TTS_ZIP_URL}" "${TTS_SOURCE_DIR}"
    fi
fi

if [[ ${VALIDATE_T2I} -eq 1 ]]; then
    validate_t2i "${T2I_MODEL_DIR}"
fi
if [[ ${VALIDATE_STT} -eq 1 ]]; then
    validate_stt "${STT_MODEL_DIR}"
fi
if [[ ${VALIDATE_TTS} -eq 1 ]]; then
    validate_tts_source_bundle "${TTS_SOURCE_DIR}"
fi

if [[ ${GENERATE_TTS_QNN} -eq 1 ]]; then
    if [[ ${FORCE_DOWNLOAD} -eq 1 ]]; then
        log_info "Force-download enabled for Text-to-Speech runtime; regenerating qnn bundle: ${TTS_MODEL_DIR}"
        generate_tts_qnn_bundle "${TTS_SOURCE_DIR}" "${TTS_MODEL_DIR}"
    elif has_tts_runtime_artifacts "${TTS_MODEL_DIR}"; then
        log_info "Skipping TTS qnn generation; runtime artifacts already present: ${TTS_MODEL_DIR}"
    else
        generate_tts_qnn_bundle "${TTS_SOURCE_DIR}" "${TTS_MODEL_DIR}"
    fi
fi

log_info ""
log_info "==================================================================="
if [[ ${FAILURES} -eq 0 ]]; then
    log_info "Model generation/acquisition checks completed successfully"
    log_info "==================================================================="
    exit 0
else
    log_error "Model generation/acquisition checks failed (${FAILURES} issue(s))"
    log_info "==================================================================="
    exit 1
fi
