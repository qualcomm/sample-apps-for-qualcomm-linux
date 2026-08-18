#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------
# Remote execution helpers for GenAI Studio automation.
# Provides SSH/SCP wrappers for remote device operations.
# ---------------------------------------------------------------------

# Global variables set by init_remote_config
REMOTE_TARGET=""
REMOTE_USER="root"
REMOTE_PASSWORD=""
SSH_BASE_FLAGS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

init_remote_config() {
    REMOTE_TARGET="$1"
    REMOTE_USER="${2:-root}"
    REMOTE_PASSWORD="${3:-}"
    
    if [[ -z "${REMOTE_TARGET}" ]]; then
        log_error "Remote target IP not provided"
        return 1
    fi
    
    log_info "Remote mode: ${REMOTE_USER}@${REMOTE_TARGET}"
    
    # Check if sshpass is available for password auth
    if [[ -n "${REMOTE_PASSWORD}" ]]; then
        if command -v sshpass >/dev/null 2>&1; then
            log_info "Using sshpass for password authentication"
        else
            log_warn "sshpass not found; password provided but will use standard ssh (may prompt)"
        fi
    fi
}

remote_exec() {
    local cmd="$1"
    
    if [[ -z "${REMOTE_TARGET}" ]]; then
        log_error "Remote target not initialized. Call init_remote_config first."
        return 1
    fi
    
    if [[ -n "${REMOTE_PASSWORD}" ]] && command -v sshpass >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        sshpass -p "${REMOTE_PASSWORD}" ssh ${SSH_BASE_FLAGS} "${REMOTE_USER}@${REMOTE_TARGET}" "${cmd}"
    else
        # shellcheck disable=SC2086
        ssh ${SSH_BASE_FLAGS} "${REMOTE_USER}@${REMOTE_TARGET}" "${cmd}"
    fi
}

remote_copy_to() {
    local src="$1"
    local dst="$2"
    
    if [[ -z "${REMOTE_TARGET}" ]]; then
        log_error "Remote target not initialized. Call init_remote_config first."
        return 1
    fi
    
    if [[ -n "${REMOTE_PASSWORD}" ]] && command -v sshpass >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        sshpass -p "${REMOTE_PASSWORD}" scp ${SSH_BASE_FLAGS} -r "${src}" "${REMOTE_USER}@${REMOTE_TARGET}:${dst}"
    else
        # shellcheck disable=SC2086
        scp ${SSH_BASE_FLAGS} -r "${src}" "${REMOTE_USER}@${REMOTE_TARGET}:${dst}"
    fi
}

remote_copy_from() {
    local src="$1"
    local dst="$2"
    
    if [[ -z "${REMOTE_TARGET}" ]]; then
        log_error "Remote target not initialized. Call init_remote_config first."
        return 1
    fi
    
    if [[ -n "${REMOTE_PASSWORD}" ]] && command -v sshpass >/dev/null 2>&1; then
        # shellcheck disable=SC2086
        sshpass -p "${REMOTE_PASSWORD}" scp ${SSH_BASE_FLAGS} -r "${REMOTE_USER}@${REMOTE_TARGET}:${src}" "${dst}"
    else
        # shellcheck disable=SC2086
        scp ${SSH_BASE_FLAGS} -r "${REMOTE_USER}@${REMOTE_TARGET}:${src}" "${dst}"
    fi
}

remote_test_connection() {
    if [[ -z "${REMOTE_TARGET}" ]]; then
        log_error "Remote target not initialized. Call init_remote_config first."
        return 1
    fi
    
    log_info "Testing connection to ${REMOTE_USER}@${REMOTE_TARGET}..."
    if remote_exec "echo 'Connection OK'"; then
        log_info "Remote connection successful"
        return 0
    else
        log_error "Failed to connect to remote target"
        return 1
    fi
}
