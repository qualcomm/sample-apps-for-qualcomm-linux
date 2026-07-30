#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Capture a reproducible "golden environment fingerprint" snapshot.
# ---------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

OUTPUT_PATH=""
PRINT_STDOUT=0

usage() {
    cat <<USAGE
Usage: $0 [--output FILE] [--print]

Options:
  --output FILE   Write fingerprint report to FILE
  --print         Also print report content to stdout
  --help          Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output)
            OUTPUT_PATH="${2:-}"
            shift 2
            ;;
        --print)
            PRINT_STDOUT=1
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

require_cmd date
require_cmd find
require_cmd sha256sum

RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)"
if [[ -z "${OUTPUT_PATH}" ]]; then
    OUTPUT_PATH="${REPO_ROOT}/test-evidence/golden-fingerprint-${RUN_TAG}.md"
fi
mkdir -p "$(dirname "${OUTPUT_PATH}")"

cd "${REPO_ROOT}"
load_versions_manifest "${REPO_ROOT}"

if [[ -f "${REPO_ROOT}/.env" ]]; then
    set -a
    # shellcheck disable=SC1091
    source "${REPO_ROOT}/.env"
    set +a
fi

cmd_or_na() {
    local cmd="$1"
    shift || true
    if command -v "${cmd}" >/dev/null 2>&1; then
        "${cmd}" "$@" 2>/dev/null || true
    else
        echo "N/A (command not found: ${cmd})"
    fi
}

line_or_na() {
    local value="$1"
    if [[ -n "${value}" ]]; then
        echo "${value}"
    else
        echo "N/A"
    fi
}

sha_file_or_na() {
    local path="$1"
    if [[ -f "${path}" ]]; then
        sha256sum "${path}" | awk '{print $1}'
    else
        echo "MISSING"
    fi
}

dir_summary_block() {
    local name="$1"
    local path="$2"
    echo "### ${name}"
    echo "- path: \`${path}\`"
    if [[ -d "${path}" ]]; then
        local real_path
        local file_count
        local total_size
        real_path="$(readlink -f "${path}" 2>/dev/null || echo "${path}")"
        file_count="$(find "${path}" -type f 2>/dev/null | wc -l | tr -d ' ')"
        total_size="$(du -sh "${path}" 2>/dev/null | awk '{print $1}')"
        echo "- realpath: \`${real_path}\`"
        echo "- file_count: ${file_count}"
        echo "- size: ${total_size}"
        echo "- sample_hashes:"
        local sample_file
        local shown=0
        while IFS= read -r sample_file; do
            shown=$((shown + 1))
            local rel="${sample_file#${path}/}"
            local hash
            hash="$(sha256sum "${sample_file}" | awk '{print $1}')"
            echo "  - \`${rel}\`: \`${hash}\`"
        done < <(find "${path}" -type f 2>/dev/null | LC_ALL=C sort | head -n 5)
        if [[ "${shown}" -eq 0 ]]; then
            echo "  - (no files found)"
        fi
    else
        echo "- status: MISSING"
    fi
    echo
}

GIT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "N/A")"
GIT_COMMIT="$(git rev-parse HEAD 2>/dev/null || echo "N/A")"
GIT_DIRTY="clean"
if ! git diff --quiet 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
    GIT_DIRTY="dirty"
fi

HOST_PRETTY_NAME="$(grep -E '^PRETTY_NAME=' /etc/os-release 2>/dev/null | cut -d= -f2- | tr -d '"' || true)"
HOST_KERNEL="$(uname -srmo 2>/dev/null || true)"
HOST_ARCH="$(uname -m 2>/dev/null || true)"
HOSTNAME_NOW="$(hostname 2>/dev/null || true)"

QAIRT_CURRENT_REAL="$(readlink -f /opt/qairt/current 2>/dev/null || echo "N/A")"
QAIRT_FLAT_PATH="${QAIRT_FLAT_LIB_DIR:-/opt/qairt/current/qairt_245_flat_libs}"

TG_MODEL_PATH="${TG_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-text}"
STT_MODEL_PATH="${STT_MODEL_HOST_DIR:-/opt/genai-studio-models/speech-to-text/whisper_tiny-qnn_context_binary-float-qualcomm_qcs9075}"
TTS_MODEL_PATH="${TTS_MODEL_HOST_DIR:-/opt/genai-studio-models/text-to-speech/melo-tts-v73/files}"
I2T_MODEL_PATH="${I2T_MODEL_HOST_DIR:-/opt/genai-studio-models/image-to-text/Lemans_LE_Gen2_QNN2_41_qwen25_vl_7B/files}"
IMG_MODEL_PATH="${IMAGEGEN_MODEL_DIR:-/opt/genai-studio-models/text-to-image/stable_diffusion_v2_1-qnn_context_binary-w8a16-qualcomm_qcs9075}"

{
    echo "# Golden Environment Fingerprint"
    echo
    echo "- generated_utc: $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
    echo "- repo_root: \`${REPO_ROOT}\`"
    echo
    echo "## 1) Source Fingerprint"
    echo
    echo "- git_branch: \`$(line_or_na "${GIT_BRANCH}")\`"
    echo "- git_commit: \`$(line_or_na "${GIT_COMMIT}")\`"
    echo "- git_worktree_state: \`${GIT_DIRTY}\`"
    echo
    echo "## 2) Host + Tooling Fingerprint"
    echo
    echo "- hostname: \`$(line_or_na "${HOSTNAME_NOW}")\`"
    echo "- os: \`$(line_or_na "${HOST_PRETTY_NAME}")\`"
    echo "- kernel: \`$(line_or_na "${HOST_KERNEL}")\`"
    echo "- arch: \`$(line_or_na "${HOST_ARCH}")\`"
    echo "- docker_version: \`$(cmd_or_na docker --version)\`"
    echo "- docker_compose_version: \`$(cmd_or_na docker compose version)\`"
    echo
    echo "## 3) QAIRT Fingerprint"
    echo
    echo "- canonical_manifest_QAIRT_VERSION: \`${QAIRT_VERSION:-N/A}\`"
    echo "- canonical_manifest_QAIRT_FLAT_LIB_DIR: \`${QAIRT_FLAT_LIB_DIR:-N/A}\`"
    echo "- /opt/qairt/current -> \`${QAIRT_CURRENT_REAL}\`"
    echo "- runtime_flat_lib_dir_used: \`${QAIRT_FLAT_PATH}\`"
    echo
    echo "### QAIRT key libraries"
    echo
    echo "| Library | sha256 |"
    echo "|---|---|"
    for lib in \
        libGenie.so \
        libQnnSystem.so \
        libQnnHtp.so \
        libQnnHtpV73Stub.so \
        libQnnHtpV73Skel.so \
        libcdsprpc.so.1.0.0 \
        libxdsprpc.so
    do
        lib_path="${QAIRT_FLAT_PATH}/${lib}"
        echo "| \`${lib}\` | \`$(sha_file_or_na "${lib_path}")\` |"
    done
    echo
    echo "## 4) Model Set Fingerprint"
    echo
    dir_summary_block "Text-Generation models" "${TG_MODEL_PATH}"
    dir_summary_block "Speech-To-Text models" "${STT_MODEL_PATH}"
    dir_summary_block "Text-To-Speech models" "${TTS_MODEL_PATH}"
    dir_summary_block "Image-To-Text models" "${I2T_MODEL_PATH}"
    dir_summary_block "Image-Generation models" "${IMG_MODEL_PATH}"
    echo "## 5) Compose + Dependency Fingerprint"
    echo
    echo "- docker-compose.yml.sha256: \`$(sha_file_or_na "${REPO_ROOT}/docker-compose.yml")\`"
    echo "- versions.env.sha256: \`$(sha_file_or_na "${REPO_ROOT}/versions.env")\`"
    if [[ -f "${REPO_ROOT}/.env" ]]; then
        echo "- .env.sha256: \`$(sha_file_or_na "${REPO_ROOT}/.env")\`"
    else
        echo "- .env.sha256: \`MISSING\`"
    fi
    echo
    echo "### Docker image fingerprint"
    echo
    echo "| Image | ID | Created |"
    echo "|---|---|---|"
    for img in \
        text-to-text:latest \
        speech-to-text:latest \
        text-to-speech:latest \
        image-to-text:latest \
        text-to-image:latest \
        orchestrator:latest
    do
        if docker image inspect "${img}" >/dev/null 2>&1; then
            img_id="$(docker image inspect -f '{{.Id}}' "${img}" 2>/dev/null || true)"
            img_created="$(docker image inspect -f '{{.Created}}' "${img}" 2>/dev/null || true)"
            echo "| \`${img}\` | \`${img_id}\` | \`${img_created}\` |"
        else
            echo "| \`${img}\` | \`MISSING\` | \`MISSING\` |"
        fi
    done
    echo
    echo "## 6) Canonical Release Keys"
    echo
    echo "Required minimum keys you defined:"
    echo
    echo "- commit: \`${GIT_COMMIT}\`"
    echo "- model_set: captured in Section 4"
    echo "- QAIRT: \`${QAIRT_VERSION:-N/A}\` + QAIRT library hashes in Section 3"
    echo "- dependencies: host/tooling + compose/image fingerprints in Sections 2 and 5"
} > "${OUTPUT_PATH}"

log_info "Golden fingerprint written: ${OUTPUT_PATH}"
if [[ "${PRINT_STDOUT}" == "1" ]]; then
    cat "${OUTPUT_PATH}"
fi
