#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# GenAI Studio Startup Automation Orchestrator
# Main entry point for automated device setup, build, and validation.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"
# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/remote.sh"

# Configuration
TARGET_HOST=""
SSH_USER="root"
SSH_PASSWORD=""
PHASES=("all")
RESUME=1
FORCE=0
FORCE_CLEAN=0
SKIP_START=0

STATE_DIR="${REPO_ROOT}/.genai-state"
RUN_TIMESTAMP="$(date -u +%Y%m%dT%H%M%SZ)"
LOG_DIR="${REPO_ROOT}/logs/startup/${RUN_TIMESTAMP}"

usage() {
    cat <<USAGE
GenAI Studio Startup Automation

Usage: $0 [OPTIONS]

Options:
  --target <ip>              Target device IP (omit for local execution)
  --user <username>          SSH username (default: root)
  --password <password>      SSH password (uses sshpass if available)
  --phase <phase> [phase...] Run specific phase(s): device-setup|model-setup|build|validate|all (default: all)
                             Multiple phases can be specified: --phase build validate
  --resume                   Resume from last checkpoint (default: true)
  --no-resume                Start from beginning, ignore checkpoints
  --force                    Force rerun of completed phases
  --force-clean              Enable cleanup hooks (docker cache, etc.)
  --skip-start               Skip 'docker compose up -d' in validate phase (assume stack already running)
  --help                     Show this help

Phases:
  device-setup    Phase 1: Detect profile, validate prerequisites, setup environment
  model-setup     Phase 2: Validate model directories and artifacts
  build           Phase 3: Build all service Docker images
  validate        Phase 4: Start stack and run validation
  all             Run all phases in order (default)

Examples:
  # Local execution, all phases
  ./scripts/genai-studio.sh

  # Remote execution
  ./scripts/genai-studio.sh --target 192.168.1.100 --user root --password mypass

  # Run only device setup phase
  ./scripts/genai-studio.sh --phase device-setup

  # Run multiple specific phases
  ./scripts/genai-studio.sh --phase build validate

  # Force rebuild everything
  ./scripts/genai-studio.sh --force --force-clean

  # Resume from last checkpoint
  ./scripts/genai-studio.sh --resume

For more information, see: docs/STARTUP_AUTOMATION_USAGE.md
USAGE
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)
            TARGET_HOST="$2"
            shift 2
            ;;
        --user)
            SSH_USER="$2"
            shift 2
            ;;
        --password)
            SSH_PASSWORD="$2"
            shift 2
            ;;
        --phase)
            shift
            PHASES=()
            while [[ $# -gt 0 ]] && [[ ! "$1" =~ ^-- ]]; do
                PHASES+=("$1")
                shift
            done
            if [[ ${#PHASES[@]} -eq 0 ]]; then
                log_error "--phase requires at least one phase argument"
                usage
                exit 1
            fi
            ;;
        --resume)
            RESUME=1
            shift
            ;;
        --no-resume)
            RESUME=0
            shift
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --force-clean)
            FORCE_CLEAN=1
            shift
            ;;
        --skip-start)
            SKIP_START=1
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

# Validate phase arguments
for phase in "${PHASES[@]}"; do
    case "${phase}" in
        device-setup|model-setup|build|validate|all)
            ;;
        *)
            log_error "Invalid phase: ${phase}"
            log_error "Valid phases: device-setup, model-setup, build, validate, all"
            exit 1
            ;;
    esac
done

# Setup logging
ensure_dirs "${LOG_DIR}" "${STATE_DIR}"
MAIN_LOG="${LOG_DIR}/genai-studio.log"
exec > >(tee -a "${MAIN_LOG}") 2>&1

log_info "==================================================================="
log_info "GenAI Studio Startup Automation"
log_info "==================================================================="
log_info "Run timestamp: ${RUN_TIMESTAMP}"
log_info "Log directory: ${LOG_DIR}"
log_info "State directory: ${STATE_DIR}"
log_info "Phases: ${PHASES[*]}"
log_info "Resume: ${RESUME}"
log_info "Force: ${FORCE}"

# Remote mode setup (future enhancement - not yet implemented)
if [[ -n "${TARGET_HOST}" ]]; then
    log_error "Remote execution not yet implemented"
    log_error "Please run this script directly on the target device for now"
    log_error ""
    log_error "To use on remote device:"
    log_error "  1. Copy repository to target device"
    log_error "  2. SSH to target device"
    log_error "  3. Run: ./scripts/genai-studio.sh"
    exit 1
else
    log_info "Mode: LOCAL"
fi

log_info "==================================================================="

# Phase execution tracking
declare -A PHASE_RESULTS

cleanup_on_interrupt() {
    local sig="${1:-INT}"
    log_warn ""
    log_warn "Received ${sig}; stopping running phase processes..."

    local bg_pids
    bg_pids="$(jobs -pr || true)"
    if [[ -n "${bg_pids}" ]]; then
        # Stop background phase wrappers started by this script.
        kill -TERM ${bg_pids} 2>/dev/null || true
        sleep 1
        kill -KILL ${bg_pids} 2>/dev/null || true
    fi

    # Best-effort cleanup for common long-running descendants.
    pkill -TERM -f "scripts/phases/model_gen.sh|scripts/phases/build-stack.sh|qnn_model_generation.py|docker build|buildx" 2>/dev/null || true
    sleep 1
    pkill -KILL -f "scripts/phases/model_gen.sh|scripts/phases/build-stack.sh|qnn_model_generation.py|docker build|buildx" 2>/dev/null || true

    log_warn "Interrupted. Exiting with code 130."
    exit 130
}

trap 'cleanup_on_interrupt INT' INT
trap 'cleanup_on_interrupt TERM' TERM

# Helper to run a phase
run_phase() {
    local phase_name="$1"
    local phase_script="$2"
    shift 2
    local phase_args=("$@")
    
    log_info ""
    log_info "-------------------------------------------------------------------"
    log_info "Executing Phase: ${phase_name}"
    log_info "-------------------------------------------------------------------"
    
    # Check if phase already completed
    if [[ ${RESUME} -eq 1 ]] && [[ ${FORCE} -eq 0 ]] && is_phase_done "${phase_name}" "${STATE_DIR}"; then
        log_info "Phase ${phase_name} already completed (checkpoint found)"
        log_info "Use --force to rerun, or --no-resume to start fresh"
        PHASE_RESULTS["${phase_name}"]="SKIPPED (checkpoint)"
        return 0
    fi
    
    # Clear checkpoint if forcing
    if [[ ${FORCE} -eq 1 ]]; then
        clear_phase "${phase_name}" "${STATE_DIR}"
    fi
    
    # Execute phase script
    local phase_log="${LOG_DIR}/${phase_name}.log"
    log_info "Phase log: ${phase_log}"
    
    set +e
    bash "${phase_script}" "${phase_args[@]}" 2>&1 | tee "${phase_log}"
    local phase_exit_code=$?
    set -e

    if [[ ${phase_exit_code} -eq 0 ]]; then
        log_info "Phase ${phase_name} completed successfully"
        mark_phase_done "${phase_name}" "${STATE_DIR}"
        PHASE_RESULTS["${phase_name}"]="PASS"
        return 0
    else
        # Special handling for exit code 2 (docker group refresh needed)
        if [[ ${phase_exit_code} -eq 2 ]]; then
            log_warn "Phase ${phase_name} requires session refresh (docker group activation)"
            log_warn "Please run ONE of the following:"
            log_warn "  1. newgrp docker  # Activate docker group in new shell"
            log_warn "  2. Log out and log back in"
            log_warn "  3. Rerun this script after group activation"
            log_warn ""
            log_warn "Then resume with: ./scripts/genai-studio.sh --resume"
            PHASE_RESULTS["${phase_name}"]="NEEDS_REFRESH"
            return 2
        fi
        
        log_error "Phase ${phase_name} failed"
        PHASE_RESULTS["${phase_name}"]="FAIL"
        return 1
    fi
}

# Execute phases based on selection
FAILURES=0

# Helper to execute a single phase
execute_single_phase() {
    local phase="$1"
    
    case "${phase}" in
        device-setup)
            local device_setup_args=()
            if [[ ${FORCE} -eq 1 ]]; then
                device_setup_args+=("--force")
            fi
            if run_phase "device-setup" "${REPO_ROOT}/scripts/phases/device-setup.sh" "${device_setup_args[@]}"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi
            ;;
        
        model-setup)
            if run_phase "model-setup" "${REPO_ROOT}/scripts/phases/model_gen.sh"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi
            ;;
        
        build)
            BUILD_ARGS=()
            if [[ ${FORCE} -eq 1 ]]; then
                BUILD_ARGS+=("--force")
            fi
            if [[ ${FORCE_CLEAN} -eq 1 ]]; then
                BUILD_ARGS+=("--force-clean")
            fi
            if run_phase "build" "${REPO_ROOT}/scripts/phases/build-stack.sh" "${BUILD_ARGS[@]}"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi
            ;;
        
        validate)
            VALIDATE_ARGS=()
            if [[ ${SKIP_START} -eq 1 ]]; then
                VALIDATE_ARGS+=("--skip-start")
            fi
            if run_phase "validate" "${REPO_ROOT}/scripts/phases/validate-stack.sh" "${VALIDATE_ARGS[@]}"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi
            ;;
        
        all)
            # Run all phases in order, with model-setup and build in parallel.
            if execute_single_phase "device-setup"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi

            log_info ""
            log_info "-------------------------------------------------------------------"
            log_info "Executing Parallel Phases: model-setup + build"
            log_info "-------------------------------------------------------------------"

            local model_pid=""
            local build_pid=""
            local model_rc=0
            local build_rc=0

            (
                if execute_single_phase "model-setup"; then
                    exit 0
                else
                    exit $?
                fi
            ) &
            model_pid=$!

            (
                if execute_single_phase "build"; then
                    exit 0
                else
                    exit $?
                fi
            ) &
            build_pid=$!

            if wait "${model_pid}"; then
                model_rc=0
            else
                model_rc=$?
            fi

            if wait "${build_pid}"; then
                build_rc=0
            else
                build_rc=$?
            fi

            # Docker group refresh takes priority to preserve existing behavior.
            if [[ ${build_rc} -eq 2 || ${model_rc} -eq 2 ]]; then
                return 2
            fi
            if [[ ${model_rc} -ne 0 ]]; then
                return "${model_rc}"
            fi
            if [[ ${build_rc} -ne 0 ]]; then
                return "${build_rc}"
            fi
            
            if execute_single_phase "validate"; then
                :
            else
                local rc=$?
                return "${rc}"
            fi
            ;;
    esac
    
    return 0
}

# Execute requested phases
for phase in "${PHASES[@]}"; do
    if execute_single_phase "${phase}"; then
        :
    else
        exit_code=$?
        
        # Exit immediately if docker group refresh needed (exit code 2)
        if [[ ${exit_code} -eq 2 ]]; then
            log_error ""
            log_error "Docker group refresh required. Exiting."
            log_error "After activating docker group, resume with: ./scripts/genai-studio.sh --resume"
            exit 2
        fi
        
        FAILURES=$((FAILURES + 1))
        # Stop on first failure unless --force
        if [[ ${FORCE} -eq 0 ]]; then
            break
        fi
    fi
done

# Print summary
log_info ""
log_info "==================================================================="
log_info "Execution Summary"
log_info "==================================================================="
print_summary PHASE_RESULTS

if [[ ${FAILURES} -eq 0 ]]; then
    ACCESS_HOST="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
    if [[ -z "${ACCESS_HOST}" ]]; then
        ACCESS_HOST="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}' || true)"
    fi
    ACCESS_HOST="${ACCESS_HOST:-localhost}"

    log_info ""
    log_info "SUCCESS: All phases completed successfully"
    log_info ""
    log_info "Next steps:"
    log_info "  - Access orchestrator: http://${ACCESS_HOST}:8090"
    log_info "  - View logs: ${LOG_DIR}"
    log_info "  - Test endpoints: curl http://${ACCESS_HOST}:8090/health"
    log_info ""
    exit 0
else
    log_error ""
    log_error "FAILURE: ${FAILURES} phase(s) failed"
    log_error ""
    log_error "Troubleshooting:"
    log_error "  - Review logs in: ${LOG_DIR}"
    log_error "  - Check phase-specific logs for details"
    log_error "  - See: docs/TROUBLESHOOTING_GUIDE.md"
    log_error "  - See: docs/STARTUP_AUTOMATION_USAGE.md"
    log_error ""
    log_error "To resume from last successful phase:"
    log_error "  ./scripts/genai-studio.sh --resume"
    log_error ""
    exit 1
fi
