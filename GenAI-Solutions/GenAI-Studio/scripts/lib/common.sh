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
