#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# Phase 2: Model Setup
# Validates required model directories and artifacts per service.
#we will tackle this in the next pass
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

STATE_DIR="${REPO_ROOT}/.genai-state"
DOWNLOAD_T2I=1
DOWNLOAD_STT=1
DOWNLOAD_I2T=1

T2I_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/stable_diffusion_v2_1/releases/v0.50.2/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075.zip"
STT_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/whisper_tiny/releases/v0.50.2/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075.zip"
I2T_ZIP_URL="https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/qwen2_5_vl_7b_instruct/releases/v0.59.0/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075.zip"
T2I_TOKENIZER_VOCAB_URL="https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/vocab.json"
T2I_TOKENIZER_MERGES_URL="https://huggingface.co/sd-research/stable-diffusion-2-1-base/resolve/main/tokenizer/merges.txt"

#for this maybe we can add a provision option?? considering it is not yet in the can automate stage,
#we can maybe provision some of the models
#also the file structure is optional so if we are just validating we need to do better
usage() {
    cat <<USAGE
Usage: $0 [OPTIONS]

Phase 2: Model Setup
- Validate model directories for all services
- Check for required model artifacts
- Report missing/restricted assets

Options:
  --download-t2i        Download and extract Text-to-Image model bundle (SD2.1)
  --download-stt        Download and extract Speech-to-Text model bundle (whisper-tiny)
  --download-i2t        Download and prepare Image-to-Text model bundle (Qwen2.5-VL)
  --download-all        Download Text-to-Image, Speech-to-Text, and Image-to-Text bundles
  --download-none       Skip all model downloads
  --skip-download-t2i   Skip Text-to-Image download
  --skip-download-stt   Skip Speech-to-Text download
  --skip-download-i2t   Skip Image-to-Text download
  --help     Show this help

NOTE: Model directory structure is flexible. This script validates
      the default paths, but services can use custom paths via
      environment variables in docker-compose.yml or .env file.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --download-t2i)
            DOWNLOAD_T2I=1
            shift
            ;;
        --download-stt)
            DOWNLOAD_STT=1
            shift
            ;;
        --download-all)
            DOWNLOAD_T2I=1
            DOWNLOAD_STT=1
            DOWNLOAD_I2T=1
            shift
            ;;
        --download-none)
            DOWNLOAD_T2I=0
            DOWNLOAD_STT=0
            DOWNLOAD_I2T=0
            shift
            ;;
        --skip-download-t2i)
            DOWNLOAD_T2I=0
            shift
            ;;
        --skip-download-stt)
            DOWNLOAD_STT=0
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

log_info "==================================================================="
log_info "Phase 2: Model Setup Validation"
log_info "==================================================================="

FAILURES=0

download_zip_model_bundle() {
    local service_name="$1"
    local bundle_url="$2"
    local dest_parent="$3"
    local expected_dir="$4"

    require_cmd wget
    require_cmd unzip

    mkdir -p "${dest_parent}"
    local zip_name
    zip_name="$(basename "${bundle_url}")"
    local tmp_zip="/tmp/${zip_name}"

    log_info ""
    log_info "Downloading ${service_name} bundle..."
    log_info "  URL: ${bundle_url}"
    log_info "  ZIP: ${tmp_zip}"
    log_info "  DEST: ${dest_parent}"

    wget -O "${tmp_zip}" "${bundle_url}"
    unzip -o "${tmp_zip}" -d "${dest_parent}"

    if [[ -d "${expected_dir}" ]]; then
        log_info "  [PASS] ${service_name} bundle ready: ${expected_dir}"
    else
        log_warn "  [WARN] ${service_name} expected directory not found after unzip: ${expected_dir}"
    fi
}

download_t2i_tokenizer_if_missing() {
    local model_dir="$1"
    require_cmd wget

    local tokenizer_dir="${model_dir}/tokenizer"
    mkdir -p "${tokenizer_dir}"

    if [[ ! -f "${tokenizer_dir}/vocab.json" ]]; then
        log_info "Downloading T2I tokenizer vocab.json..."
        wget -O "${tokenizer_dir}/vocab.json" "${T2I_TOKENIZER_VOCAB_URL}"
    fi
    if [[ ! -f "${tokenizer_dir}/merges.txt" ]]; then
        log_info "Downloading T2I tokenizer merges.txt..."
        wget -O "${tokenizer_dir}/merges.txt" "${T2I_TOKENIZER_MERGES_URL}"
    fi
}

copy_stt_vad_if_available() {
    local stt_model_dir="$1"
    local vad_dst="${stt_model_dir}/libnnvad_model.so"

    if [[ -f "${vad_dst}" ]]; then
        return 0
    fi

    local candidates=(
        "${REPO_ROOT}/core-services/speech-to-text/whisper_sdk/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so"
        "/opt/qcom/qpm/VoiceAI_ASR/2.6.0.0/whisper_sdk/libs/npu/rpc_libraries/assets/aarch64_linux/libnnvad_model.so"
    )

    for src in "${candidates[@]}"; do
        if [[ -f "${src}" ]]; then
            cp -f "${src}" "${vad_dst}"
            log_info "Copied STT VAD asset from: ${src}"
            return 0
        fi
    done

    log_warn "STT VAD asset not found in known SDK paths."
    log_warn "Expected one of:"
    for src in "${candidates[@]}"; do
        log_warn "  - ${src}"
    done
    log_warn "You may need to stage whisper_sdk manually, then rerun --download-stt."
    return 0
}

sync_dir_contents() {
    local src_dir="$1"
    local dst_dir="$2"
    mkdir -p "${dst_dir}"
    if command -v rsync >/dev/null 2>&1; then
        rsync -a --delete "${src_dir}/" "${dst_dir}/"
    else
        rm -rf "${dst_dir:?}/"*
        cp -a "${src_dir}/." "${dst_dir}/"
    fi
}

prepare_i2t_runtime_layout() {
    local i2t_runtime_dir="$1"

    # Match the runtime-ready layout we validated previously.
    if [[ -d "${i2t_runtime_dir}/backups_20260804_062541" ]]; then
        for cfg in genie_config.json img-enc-htp.json text-encoder.json; do
            local src_cfg="${i2t_runtime_dir}/backups_20260804_062541/${cfg}"
            if [[ -f "${src_cfg}" ]]; then
                cp -f "${src_cfg}" "${i2t_runtime_dir}/${cfg}"
                log_info "Promoted I2T config from backup: ${cfg}"
            fi
        done
    fi

    if [[ ! -d "${i2t_runtime_dir}/inputs" ]] && [[ -d "${i2t_runtime_dir}/sample_inputs" ]]; then
        cp -a "${i2t_runtime_dir}/sample_inputs" "${i2t_runtime_dir}/inputs"
        log_info "Created I2T inputs/ from sample_inputs/"
    fi
    mkdir -p "${i2t_runtime_dir}/uploads"

    if [[ ! -f "${i2t_runtime_dir}/libGenie.so" ]]; then
        local candidates=(
            "${I2T_MODEL_DIR:-}/libGenie.so"
            "/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files/libGenie.so"
            "/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075/files/libGenie.so"
            "/opt/genai-studio-models/text-to-text/llama_v3_2_3b_instruct_ssd-genie-w4a16-qualcomm_qcs9075/libGenie.so"
        )
        local src_lib=""
        for cand in "${candidates[@]}"; do
            if [[ -f "${cand}" ]]; then
                src_lib="${cand}"
                break
            fi
        done
        if [[ -n "${src_lib}" ]]; then
            cp -f "${src_lib}" "${i2t_runtime_dir}/libGenie.so"
            log_info "Injected I2T libGenie.so from: ${src_lib}"
        else
            log_warn "I2T libGenie.so not found in known locations; stage it manually if needed."
        fi
    fi
}

if [[ ${DOWNLOAD_T2I} -eq 1 ]] || [[ ${DOWNLOAD_STT} -eq 1 ]] || [[ ${DOWNLOAD_I2T} -eq 1 ]]; then
    log_info ""
    log_info "Requested model downloads:"
    log_info "  - Text-to-Image download: $(if [[ ${DOWNLOAD_T2I} -eq 1 ]]; then echo yes; else echo no; fi)"
    log_info "  - Speech-to-Text download: $(if [[ ${DOWNLOAD_STT} -eq 1 ]]; then echo yes; else echo no; fi)"
    log_info "  - Image-to-Text download: $(if [[ ${DOWNLOAD_I2T} -eq 1 ]]; then echo yes; else echo no; fi)"

    if [[ ${DOWNLOAD_T2I} -eq 1 ]]; then
        T2I_PARENT_DIR="/opt/genai-studio-models/text-to-image"
        T2I_EXPECTED_DIR="${T2I_PARENT_DIR}/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075"
        download_zip_model_bundle "Text-to-Image" "${T2I_ZIP_URL}" "${T2I_PARENT_DIR}" "${T2I_EXPECTED_DIR}"
        if [[ -d "${T2I_EXPECTED_DIR}" ]]; then
            download_t2i_tokenizer_if_missing "${T2I_EXPECTED_DIR}"
        fi
    fi

    if [[ ${DOWNLOAD_STT} -eq 1 ]]; then
        STT_PARENT_DIR="/opt/genai-studio-models/speech-to-text"
        STT_EXPECTED_DIR="${STT_PARENT_DIR}/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075"
        download_zip_model_bundle "Speech-to-Text" "${STT_ZIP_URL}" "${STT_PARENT_DIR}" "${STT_EXPECTED_DIR}"
        if [[ -d "${STT_EXPECTED_DIR}" ]]; then
            copy_stt_vad_if_available "${STT_EXPECTED_DIR}"
        fi
    fi

    if [[ ${DOWNLOAD_I2T} -eq 1 ]]; then
        I2T_PARENT_DIR="/opt/genai-studio-models"
        I2T_ZIP_DIR="${I2T_PARENT_DIR}/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075"
        I2T_RUNTIME_DIR="${I2T_MODEL_DIR:-/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075}"

        download_zip_model_bundle "Image-to-Text" "${I2T_ZIP_URL}" "${I2T_PARENT_DIR}" "${I2T_ZIP_DIR}"
        if [[ -d "${I2T_ZIP_DIR}" ]]; then
            sync_dir_contents "${I2T_ZIP_DIR}" "${I2T_RUNTIME_DIR}"
            prepare_i2t_runtime_layout "${I2T_RUNTIME_DIR}"
            log_info "  [PASS] I2T runtime directory prepared: ${I2T_RUNTIME_DIR}"
        else
            log_warn "  [WARN] I2T zip directory not found after unzip: ${I2T_ZIP_DIR}"
        fi
    fi
fi

# Text-to-Text (Genie)
# Note: Default model shown here, but any Genie-compatible model can be used
log_info ""
log_info "Checking Text-to-Text models..."
TG_MODEL_DIR="${TG_MODEL_DIR:-/opt/genai-studio-models/text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075}"
if [[ -d "${TG_MODEL_DIR}" ]]; then
    log_info "  [PASS] Model directory exists: ${TG_MODEL_DIR}"
    
    # Check for genie_config.json
    GENIE_CONFIG="${TG_MODEL_DIR}/genie_config.json"
    if [[ -f "${GENIE_CONFIG}" ]]; then
        log_info "  [PASS] genie_config.json found"
    else
        log_error "  [FAIL] genie_config.json NOT FOUND: ${GENIE_CONFIG}"
        FAILURES=$((FAILURES + 1))
    fi
    
    # Check for libGenie.so
    if [[ -f "${TG_MODEL_DIR}/libGenie.so" ]]; then
        log_info "  [PASS] libGenie.so found"
    else
        log_warn "  [WARN] libGenie.so NOT FOUND (may be in a different location)"
        log_warn "         Some models may not require libGenie.so in the model directory"
    fi
else
    log_error "  [FAIL] Model directory NOT FOUND: ${TG_MODEL_DIR}"
    FAILURES=$((FAILURES + 1))
fi

# Image-to-Text (Qwen2.5-VL)
log_info ""
log_info "Checking Image-to-Text models..."
I2T_MODEL_DIR="${I2T_MODEL_DIR:-/opt/genai-studio-models/image-to-text/qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075}"
if [[ -d "${I2T_MODEL_DIR}" ]]; then
    log_info "  [PASS] Model directory exists: ${I2T_MODEL_DIR}"
    
    # Check for libGenie.so
    if [[ -f "${I2T_MODEL_DIR}/libGenie.so" ]]; then
        log_info "  [PASS] libGenie.so found"
    else
        log_error "  [FAIL] libGenie.so NOT FOUND"
        log_error "         libGenie.so is required for Image-to-Text service"
        log_error "         If you have libGenie.so elsewhere, copy it to: ${I2T_MODEL_DIR}/"
        FAILURES=$((FAILURES + 1))
    fi
    
    # Check for image encoder config
    I2T_IMG_CFG=""
    for cand in image_encoder.json img-enc-htp.json; do
        if [[ -f "${I2T_MODEL_DIR}/${cand}" ]]; then
            I2T_IMG_CFG="${cand}"
            break
        fi
    done
    if [[ -n "${I2T_IMG_CFG}" ]]; then
        log_info "  [PASS] Image encoder config found: ${I2T_IMG_CFG}"
    else
        log_error "  [FAIL] Image encoder config NOT FOUND (expected image_encoder.json or img-enc-htp.json)"
        FAILURES=$((FAILURES + 1))
    fi
    
    # Check for text generator config
    I2T_TEXT_CFG=""
    for cand in text_generator.json text-dec-htp.json qwen2_5-vl-e2t-htp.json; do
        if [[ -f "${I2T_MODEL_DIR}/${cand}" ]]; then
            I2T_TEXT_CFG="${cand}"
            break
        fi
    done
    if [[ -n "${I2T_TEXT_CFG}" ]]; then
        log_info "  [PASS] Text generator config found: ${I2T_TEXT_CFG}"
    else
        log_error "  [FAIL] Text generator config NOT FOUND (expected text_generator.json or text-dec-htp.json or qwen2_5-vl-e2t-htp.json)"
        FAILURES=$((FAILURES + 1))
    fi
else
    log_error "  [FAIL] Model directory NOT FOUND: ${I2T_MODEL_DIR}"
    FAILURES=$((FAILURES + 1))
fi

# Text-to-Image (Stable Diffusion)
# Requires 3 QNN context binary files: text_encoder, unet, vae
log_info ""
log_info "Checking Text-to-Image models..."
IMG_MODEL_DIR="${IMAGEGEN_MODEL_DIR:-/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075}"
if [[ -d "${IMG_MODEL_DIR}" ]]; then
    log_info "  [PASS] Model directory exists: ${IMG_MODEL_DIR}"
    
    # Check for QNN context binaries (need 3: text_encoder, unet, vae)
    TEXT_ENC_BINS=$(find "${IMG_MODEL_DIR}" -maxdepth 1 -type f -name "*text_encoder*.bin" 2>/dev/null | wc -l)
    UNET_BINS=$(find "${IMG_MODEL_DIR}" -maxdepth 1 -type f -name "*unet*.bin" 2>/dev/null | wc -l)
    VAE_BINS=$(find "${IMG_MODEL_DIR}" -maxdepth 1 -type f -name "*vae*.bin" 2>/dev/null | wc -l)
    
    if [[ ${TEXT_ENC_BINS} -gt 0 ]] && [[ ${UNET_BINS} -gt 0 ]] && [[ ${VAE_BINS} -gt 0 ]]; then
        log_info "  [PASS] Found required QNN context binaries (text_encoder, unet, vae)"
    else
        log_error "  [FAIL] Missing required QNN context binaries"
        [[ ${TEXT_ENC_BINS} -eq 0 ]] && log_error "         Missing: text_encoder*.bin"
        [[ ${UNET_BINS} -eq 0 ]] && log_error "         Missing: unet*.bin"
        [[ ${VAE_BINS} -eq 0 ]] && log_error "         Missing: vae*.bin"
        FAILURES=$((FAILURES + 1))
    fi
    
    # Check for tokenizer
    if [[ -d "${IMG_MODEL_DIR}/tokenizer" ]]; then
        log_info "  [PASS] Tokenizer directory found"
    else
        log_warn "  [WARN] Tokenizer directory not found (may be optional)"
    fi
else
    log_error "  [FAIL] Model directory NOT FOUND: ${IMG_MODEL_DIR}"
    FAILURES=$((FAILURES + 1))
fi

# Speech-to-Text (Whisper)
# Requires encoder.bin, decoder.bin, vocab.bin, and VAD model
log_info ""
log_info "Checking Speech-to-Text models..."
STT_MODEL_DIR="${STT_MODEL_HOST_DIR:-/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075}"
if [[ -d "${STT_MODEL_DIR}" ]]; then
    log_info "  [PASS] Model directory exists: ${STT_MODEL_DIR}"
    
    # Check for QNN binaries (encoder, decoder, vocab)
    ENCODER_BINS=$(find "${STT_MODEL_DIR}" -maxdepth 2 -type f \( -name "encoder.bin" -o -name "*encoder*.bin" \) 2>/dev/null | wc -l)
    DECODER_BINS=$(find "${STT_MODEL_DIR}" -maxdepth 2 -type f \( -name "decoder.bin" -o -name "*decoder*.bin" \) 2>/dev/null | wc -l)
    VOCAB_BINS=$(find "${STT_MODEL_DIR}" -maxdepth 2 -type f -name "vocab.bin" 2>/dev/null | wc -l)
    
    if [[ ${ENCODER_BINS} -gt 0 ]] && [[ ${DECODER_BINS} -gt 0 ]] && [[ ${VOCAB_BINS} -gt 0 ]]; then
        log_info "  [PASS] Found required QNN binary files (encoder, decoder, vocab)"
    else
        log_error "  [FAIL] Missing required QNN binary files"
        [[ ${ENCODER_BINS} -eq 0 ]] && log_error "         Missing: encoder.bin"
        [[ ${DECODER_BINS} -eq 0 ]] && log_error "         Missing: decoder.bin"
        [[ ${VOCAB_BINS} -eq 0 ]] && log_error "         Missing: vocab.bin"
        FAILURES=$((FAILURES + 1))
    fi
    
    # Check for VAD model (libnnvad_model.so)
    VAD_FOUND=0
    for vad_path in "${STT_MODEL_DIR}/libnnvad_model.so" "/opt/asr-assets/libnnvad_model.so"; do
        if [[ -f "${vad_path}" ]]; then
            log_info "  [PASS] VAD model found: ${vad_path}"
            VAD_FOUND=1
            break
        fi
    done
    
    if [[ ${VAD_FOUND} -eq 0 ]]; then
        log_warn "  [WARN] VAD model (libnnvad_model.so) not found"
        log_warn "         Expected in: ${STT_MODEL_DIR}/ or /opt/asr-assets/"
        log_warn "         Service may fail without VAD model"
    fi
else
    log_error "  [FAIL] Model directory NOT FOUND: ${STT_MODEL_DIR}"
    FAILURES=$((FAILURES + 1))
fi

# Text-to-Speech (MeloTTS)
log_info ""
log_info "Checking Text-to-Speech models..."
TTS_MODEL_DIR="${TTS_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-speech/melo-tts-v73/files}"
if [[ -d "${TTS_MODEL_DIR}" ]]; then
    log_info "  [PASS] Model directory exists: ${TTS_MODEL_DIR}"
    
    # Check for QNN models
    mapfile -t TTS_QNNS < <(find "${TTS_MODEL_DIR}" -maxdepth 1 -type f -name "*.qnn*" 2>/dev/null | sort)
    if [[ "${#TTS_QNNS[@]}" -eq 0 ]]; then
        log_error "  [FAIL] No .qnn model files found"
        FAILURES=$((FAILURES + 1))
    else
        log_info "  [PASS] Found ${#TTS_QNNS[@]} QNN model files"
    fi
else
    log_error "  [FAIL] Model directory NOT FOUND: ${TTS_MODEL_DIR}"
    FAILURES=$((FAILURES + 1))
fi

# Summary
log_info ""
log_info "==================================================================="
if [[ ${FAILURES} -eq 0 ]]; then
    log_info "Phase 2: Model Setup Validation - COMPLETE"
    log_info "==================================================================="
    log_info ""
    log_info "All required model artifacts validated successfully."
    exit 0
else
    log_error "Phase 2: Model Setup Validation - FAILED"
    log_info "==================================================================="
    log_error ""
    log_error "Found ${FAILURES} missing or invalid model artifact(s)."
    log_error ""
    log_error "REQUIRED ACTIONS:"
    log_error "  1. Ensure all model directories exist under /opt/genai-studio-models/"
    log_error "  2. Download/copy required model artifacts to their respective directories"
    log_error "  3. For restricted/licensed models, follow vendor instructions"
    log_error ""
    log_error "Model directory structure:"
    log_error "  /opt/genai-studio-models/"
    log_error "    ├── text-to-text/qwen3_4b-genie-w4a16-qualcomm_qcs9075/"
    log_error "    ├── qwen2_5_vl_7b_instruct-genie-w4a16-qualcomm_qcs9075/"
    log_error "    ├── text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075/"
    log_error "    ├── speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075/"
    log_error "    └── text-to-speech/melo-tts-v73/files/"
    log_error ""
    log_error "See repository documentation for model acquisition instructions."
    exit 1
fi
