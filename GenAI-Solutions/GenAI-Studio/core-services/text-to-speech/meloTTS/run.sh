#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
#
# Run the MeloTTS C++ service (tts-service) using the melo_sdk.
#
# The service loads a single bundled .qnn model file produced by:
#   model_conversion_scripts/melo/generate_qnn_model_en.sh
#
# Expected model file (English):
#   /opt/TTS_binary/MeloTTS/melo_en.64_bit.qnn_v2.33.0.qnn
#
# The service also needs libtts_impl_skel.so deployed on the CDSP.
# ADSP_LIBRARY_PATH must include the directory containing that .so.
#
# Usage:
#   bash run.sh [--model-path <path>] [--language <lang>] [--port <port>]
#               [--speaking-rate <r>] [--pitch <p>] [--volume-gain <g>]
#               [--sample-rate <hz>]
# ---------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="${TTS_BINARY:-${SCRIPT_DIR}/src/build/tts-service}"
if [[ ! -f "${BINARY}" && -x /usr/bin/tts-service ]]; then
    BINARY="/usr/bin/tts-service"
fi
SDK_LIB_DIR="${SCRIPT_DIR}/melo_sdk/libs/npu/rpc_libraries/linux/aarch64"
if [[ ! -d "${SDK_LIB_DIR}" ]]; then
    SDK_LIB_DIR="/opt/melo-sdk-runtime/libs/npu/rpc_libraries/linux/aarch64"
fi
if [[ ! -d "${SDK_LIB_DIR}" ]]; then
    SDK_LIB_DIR="/usr/lib"
fi

# ---- Defaults ---------------------------------------------------------------
MODEL_PATH="/opt/TTS_binary/MeloTTS"
LANGUAGE="English"
PORT=8083
SPEAKING_RATE="1.0"
PITCH="0.0"
VOLUME_GAIN="0.0"
SAMPLE_RATE="44100"

# ---- Parse arguments --------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --model-path)    MODEL_PATH="$2";    shift 2 ;;
        --language)      LANGUAGE="$2";      shift 2 ;;
        --port)          PORT="$2";          shift 2 ;;
        --speaking-rate) SPEAKING_RATE="$2"; shift 2 ;;
        --pitch)         PITCH="$2";         shift 2 ;;
        --volume-gain)   VOLUME_GAIN="$2";   shift 2 ;;
        --sample-rate)   SAMPLE_RATE="$2";   shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--model-path <path>] [--language <lang>] [--port <port>]"
            echo "          [--speaking-rate <r>] [--pitch <p>] [--volume-gain <g>]"
            echo "          [--sample-rate <hz>]"
            exit 0
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
done

# ---- Verify binary exists ---------------------------------------------------
if [[ ! -x "${BINARY}" ]]; then
    echo "[run] Binary not found: ${BINARY}"
    echo "[run] Run 'bash build.sh' first."
    exit 1
fi

# ---- Set library paths ------------------------------------------------------
#
# ADSP_LIBRARY_PATH  – CDSP searches this path for libtts_impl_skel.so
#   The model directory is included so the skel library can be co-located
#   with the model file (common deployment pattern).
#
# LD_LIBRARY_PATH    – host linker searches this for libtts.so
#
MODEL_DIR="$(dirname "${MODEL_PATH}")"
if [[ -d "${MODEL_PATH}" ]]; then
    MODEL_DIR="${MODEL_PATH}"
fi
QAIRT_FLAT_LIB_DIR="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"

dedupe_colon_path() {
    local input="$1"
    local output=""
    local part=""
    IFS=':' read -r -a parts <<< "${input}"
    for part in "${parts[@]}"; do
        [[ -z "${part}" ]] && continue
        case ":${output}:" in
            *:"${part}":*) ;;
            *)
                if [[ -z "${output}" ]]; then
                    output="${part}"
                else
                    output="${output}:${part}"
                fi
                ;;
        esac
    done
    printf '%s' "${output}"
}

dedupe_semicolon_path() {
    local input="$1"
    local output=""
    local part=""
    IFS=';' read -r -a parts <<< "${input}"
    for part in "${parts[@]}"; do
        [[ -z "${part}" ]] && continue
        case ";${output};" in
            *";${part};"*) ;;
            *)
                if [[ -z "${output}" ]]; then
                    output="${part}"
                else
                    output="${output};${part}"
                fi
                ;;
        esac
    done
    printf '%s' "${output}"
}

normalize_adsp_path() {
    local input="$1"
    # ADSP path separator is ';'. Normalize accidental ':' separators first.
    printf '%s' "${input}" | tr ':' ';'
}

# removed guards against missing fastrpc libs here

DEFAULT_ADSP_LIBRARY_PATH="${MODEL_DIR};/usr/lib/rfsa/adsp;/usr/lib/dsp;/usr/lib/dsp/cdsp;/dsp;/usr/lib/dsp/cdsp1"
if [[ -d "${QAIRT_FLAT_LIB_DIR}" ]]; then
    DEFAULT_ADSP_LIBRARY_PATH="${QAIRT_FLAT_LIB_DIR};${DEFAULT_ADSP_LIBRARY_PATH}"
fi
if [[ -n "${ADSP_LIBRARY_PATH:-}" ]]; then
    export ADSP_LIBRARY_PATH="$(dedupe_semicolon_path "$(normalize_adsp_path "${MODEL_DIR};${ADSP_LIBRARY_PATH}")")"
else
    export ADSP_LIBRARY_PATH="$(dedupe_semicolon_path "$(normalize_adsp_path "${DEFAULT_ADSP_LIBRARY_PATH}")")"
fi

if [[ -n "${DSP_LIBRARY_PATH:-}" ]]; then
    export DSP_LIBRARY_PATH="$(dedupe_colon_path "${MODEL_DIR}:${DSP_LIBRARY_PATH}")"
else
    export DSP_LIBRARY_PATH="${MODEL_DIR}"
fi

LD_PATH_PREFIX="${SDK_LIB_DIR}:${MODEL_DIR}:/opt/host-libs:/usr/lib:/usr/lib/aarch64-linux-gnu"
if [[ -d "${QAIRT_FLAT_LIB_DIR}" ]]; then
    LD_PATH_PREFIX="${QAIRT_FLAT_LIB_DIR}:${LD_PATH_PREFIX}"
fi
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    export LD_LIBRARY_PATH="$(dedupe_colon_path "${LD_PATH_PREFIX}:${LD_LIBRARY_PATH}")"
else
    export LD_LIBRARY_PATH="$(dedupe_colon_path "${LD_PATH_PREFIX}")"
fi

if [[ ! -e /usr/lib/dsp/fastrpc_shell_unsigned_3 ]] && [[ -e /usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3 ]]; then
    echo "[run] WARNING: /usr/lib/dsp/fastrpc_shell_unsigned_3 is missing."
    echo "[run] WARNING: This target exposes it at /usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3."
fi

if [[ ! -f /usr/lib/libtts.so ]]; then
    echo "[run] ERROR: required runtime library missing: /usr/lib/libtts.so"
    exit 1
fi
if [[ ! -f "${MODEL_DIR}/libtts_impl_skel.so" ]]; then
    echo "[run] ERROR: missing ${MODEL_DIR}/libtts_impl_skel.so"
    echo "[run] Verify TTS model payload mount contains DSP skel artifacts."
    exit 1
fi
if ! compgen -G "${MODEL_DIR}/*.qnn" > /dev/null; then
    echo "[run] ERROR: no .qnn model found under ${MODEL_DIR}"
    exit 1
fi


echo "============================================================"
echo " MeloTTS Service (melo_sdk C++ backend)"
echo "============================================================"
echo "  Binary        : ${BINARY}"
echo "  Model path    : ${MODEL_PATH}"
echo "  Language      : ${LANGUAGE}"
echo "  Port          : ${PORT}"
echo "  Speaking rate : ${SPEAKING_RATE}"
echo "  Pitch         : ${PITCH}"
echo "  Volume gain   : ${VOLUME_GAIN} dB"
echo "  Sample rate   : ${SAMPLE_RATE} Hz"
echo "------------------------------------------------------------"
echo "  ADSP_LIBRARY_PATH : ${ADSP_LIBRARY_PATH}"
echo "  DSP_LIBRARY_PATH  : ${DSP_LIBRARY_PATH}"
echo "  LD_LIBRARY_PATH   : ${LD_LIBRARY_PATH}"
echo "============================================================"

exec "${BINARY}" \
    --model-path    "${MODEL_PATH}"    \
    --language      "${LANGUAGE}"      \
    --port          "${PORT}"          \
    --speaking-rate "${SPEAKING_RATE}" \
    --pitch         "${PITCH}"         \
    --volume-gain   "${VOLUME_GAIN}"   \
    --sample-rate   "${SAMPLE_RATE}"
