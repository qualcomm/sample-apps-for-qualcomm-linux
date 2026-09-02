#!/bin/bash
# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Shared helper functions for GenAI Studio scripts.
# ---------------------------------------------------------------------

timestamp_utc() {
    date -u +"%Y-%m-%dT%H:%M:%SZ"
}

log_info() {
    echo "[$(timestamp_utc)] [INFO] $*"
}

log_warn() {
    echo "[$(timestamp_utc)] [WARN] $*" >&2
}

log_error() {
    echo "[$(timestamp_utc)] [ERROR] $*" >&2
}

die() {
    log_error "$*"
    exit 1
}

require_cmd() {
    local cmd="$1"
    command -v "${cmd}" >/dev/null 2>&1 || die "Required command not found: ${cmd}"
}

require_file() {
    local path="$1"
    [[ -f "${path}" ]] || die "Required file not found: ${path}"
}

require_dir() {
    local path="$1"
    [[ -d "${path}" ]] || die "Required directory not found: ${path}"
}

load_versions_manifest() {
    local repo_root="$1"
    local versions_file="${repo_root}/versions.env"
    if [[ -f "${versions_file}" ]]; then
        set -a
        # shellcheck disable=SC1090
        source "${versions_file}"
        set +a
        log_info "Loaded version manifest: ${versions_file}"
    else
        log_warn "Version manifest not found (using script defaults): ${versions_file}"
    fi
}

wait_for_http_ok() {
    local url="$1"
    local timeout_sec="${2:-180}"
    local interval_sec="${3:-3}"
    local deadline=$((SECONDS + timeout_sec))

    while (( SECONDS < deadline )); do
        if curl -fsS "${url}" >/dev/null 2>&1; then
            return 0
        fi
        sleep "${interval_sec}"
    done

    return 1
}

run_cmd() {
    log_info "Running: $*"
    "$@"
    local rc=$?
    if [[ ${rc} -ne 0 ]]; then
        log_error "Command failed with exit code ${rc}: $*"
    fi
    return ${rc}
}

detect_compose_cmd() {
    if docker compose version >/dev/null 2>&1; then
        echo "docker compose"
    elif command -v docker-compose >/dev/null 2>&1; then
        echo "docker-compose"
    else
        log_error "Neither 'docker compose' nor 'docker-compose' found"
        return 1
    fi
}

ensure_dirs() {
    local log_dir="${1:-logs/startup}"
    local state_dir="${2:-.genai-state}"
    mkdir -p "${log_dir}" "${state_dir}"
}

mark_phase_done() {
    local phase="$1"
    local state_dir="${2:-.genai-state}"
    mkdir -p "${state_dir}"
    touch "${state_dir}/${phase}.ok"
    log_info "Phase marked complete: ${phase}"
}

is_phase_done() {
    local phase="$1"
    local state_dir="${2:-.genai-state}"
    [[ -f "${state_dir}/${phase}.ok" ]]
}

clear_phase() {
    local phase="$1"
    local state_dir="${2:-.genai-state}"
    rm -f "${state_dir}/${phase}.ok"
    log_info "Phase checkpoint cleared: ${phase}"
}

print_summary() {
    local -n results=$1
    echo ""
    echo "═══════════════════════════════════════════════════════════════"
    echo "  GenAI Studio Startup Summary"
    echo "═══════════════════════════════════════════════════════════════"
    for phase in "${!results[@]}"; do
        printf "  %-20s : %s\n" "${phase}" "${results[${phase}]}"
    done
    echo "═══════════════════════════════════════════════════════════════"
    echo ""
}

# ---------------------------------------------------------------------
# Profile detection
# ---------------------------------------------------------------------

detect_profile() {
    # Returns: ubuntu|qli1|qli2|unknown
    local profile="unknown"
    
    if [[ -f /etc/os-release ]]; then
        # shellcheck disable=SC1091
        source /etc/os-release
        
        # Ubuntu 24.04 detection
        if [[ "${ID:-}" == "ubuntu" ]] && [[ "${VERSION_ID:-}" == "24.04" ]]; then
            profile="ubuntu"
        # QLI detection (Qualcomm Linux)
        elif [[ "${ID:-}" =~ qli|qualcomm-linux ]]; then
            # Distinguish QLI 1.x vs 2.0 by version or other signals
            if [[ "${VERSION_ID:-}" =~ ^2\. ]]; then
                profile="qli2"
            else
                profile="qli1"
            fi
        # Fallback: check for QLI-specific paths
        elif [[ -d /usr/share/qcom ]] && [[ -f /usr/lib/libQnnHtp.so ]]; then
            # Heuristic: QLI 2.0 uses newer CDI version and different paths
            if [[ -f /usr/share/qcom/sa8775p/Qualcomm/SA8775P-RIDE/dsp/cdsp/fastrpc_shell_unsigned_3 ]]; then
                profile="qli2"
            else
                profile="qli1"
            fi
        fi
    fi
    
    echo "${profile}"
}

#revalidate the cdi files for all 3 builds once again? this needs to be done by me, not the agent.
get_cdi_source_path() {
    local profile="$1"
    local repo_root="$2"
    
    case "${profile}" in
        ubuntu)
            echo "${repo_root}/cdi/2.x/docker-run-cdi-hw-acc.json"
            ;;
        qli1)
            echo "${repo_root}/cdi/1.x/docker-run-cdi-hw-acc.json"
            ;;
        qli2)
            echo "${repo_root}/cdi/2.x/docker-run-cdi-hw-acc.json"
            ;;
        *)
            echo ""
            ;;
    esac
}

get_fastrpc_shell_search_paths() {
    local profile="$1"
    
    case "${profile}" in
        ubuntu|qli1)
            echo "/usr/lib/dsp/cdsp /usr/lib/dsp"
            ;;
        qli2)
            echo "/usr/share/qcom/sa8775p/Qualcomm/SA8775P-RIDE/dsp/cdsp /usr/lib/dsp/cdsp /usr/lib/dsp"
            ;;
        *)
            echo "/usr/lib/dsp/cdsp /usr/lib/dsp"
            ;;
    esac
}

# Execute command with sudo if available (Ubuntu), without sudo if not (QLI)
run_privileged() {
    if command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        "$@"
    fi
}

# Try with sudo first, fallback to direct execution
try_privileged() {
    if sudo "$@" 2>/dev/null; then
        return 0
    elif "$@" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}
