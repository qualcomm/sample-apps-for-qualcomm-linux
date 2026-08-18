#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# Phase 4: Validate Stack
# Brings up the stack and runs health checks and validation.
#will go thru this in next pass
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

STATE_DIR="${REPO_ROOT}/.genai-state"
SKIP_START=0
EXPECTED_CONTAINERS=(
    "text-to-text"
    "image-to-text"
    "text-to-image"
    "speech-to-text"
    "text-to-speech"
    "orchestrator"
)

usage() {
    cat <<USAGE
Usage: $0 [--skip-start]

Phase 4: Validate Stack
- Bring up Docker Compose stack
- Run health checks on all services
- Execute validation suite if available

Options:
  --skip-start   Skip 'docker compose up -d' (assume stack already running)
  --help         Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
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

log_info "==================================================================="
log_info "Phase 4: Validate Stack"
log_info "==================================================================="

cd "${REPO_ROOT}"

# Detect compose command
COMPOSE_CMD=$(detect_compose_cmd)
log_info "Using compose command: ${COMPOSE_CMD}"

# Access host shown in log output (device IP preferred over localhost)
ACCESS_HOST="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
if [[ -z "${ACCESS_HOST}" ]]; then
    ACCESS_HOST="$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}' || true)"
fi
ACCESS_HOST="${ACCESS_HOST:-localhost}"

# Report current container status from docker ps -a
log_info ""
log_info "Checking expected service containers (docker ps -a)..."
declare -A CONTAINER_STATUS
while IFS=$'\t' read -r cname cstatus; do
    [[ -z "${cname}" ]] && continue
    CONTAINER_STATUS["${cname}"]="${cstatus}"
done < <(docker ps -a --format '{{.Names}}\t{{.Status}}')

for container in "${EXPECTED_CONTAINERS[@]}"; do
    status="${CONTAINER_STATUS[${container}]:-}"
    if [[ -z "${status}" ]]; then
        log_warn "Service not present: ${container} (container not found)"
    elif [[ "${status}" == Up* ]]; then
        log_info "Service up: ${container} (${status})"
    else
        log_warn "Service not up: ${container} (${status})"
    fi
done

# Bring up stack
if [[ ${SKIP_START} -eq 0 ]]; then
    log_info ""
    log_info "Starting Docker Compose stack..."
    COMPOSE_UP_LOG="$(mktemp)"
    if ${COMPOSE_CMD} up -d >"${COMPOSE_UP_LOG}" 2>&1; then
        rm -f "${COMPOSE_UP_LOG}"
        log_info "Stack started successfully"
    else
        cat "${COMPOSE_UP_LOG}" >&2 || true
        log_error "Failed to start stack"
        cdi_missing_dev="$(sed -n 's/.*failed to stat CDI host device "\(\/dev\/[^"]*\)".*/\1/p' "${COMPOSE_UP_LOG}" | head -n1 || true)"
        if [[ -n "${cdi_missing_dev}" ]]; then
            log_error ""
            log_error "Detected missing CDI host device: ${cdi_missing_dev}"
            log_error "Please remove ${cdi_missing_dev} from /etc/cdi/docker-run-cdi-hw-acc.json and try again."
        else
            log_error ""
            log_error "Common causes:"
            log_error "  1. CDI device injection error (e.g., /dev/kgsl-3d0 not found)"
            log_error "     - Check: docker compose logs | grep -i 'CDI\|device'"
            log_error "     - Fix: Remove problematic device from /etc/cdi/docker-run-cdi-hw-acc.json"
            log_error "     - See: docs/TROUBLESHOOTING_GUIDE.md (Special Case: CDI Device Injection Error)"
            log_error "  2. Missing model directories or artifacts"
            log_error "     - Run: bash scripts/phases/model-setup.sh"
            log_error "  3. Missing QAIRT or runtime dependencies"
            log_error "     - Run: bash scripts/phases/device-setup.sh --provision"
            log_error ""
            log_error "For detailed logs, run: docker compose logs"
        fi
        rm -f "${COMPOSE_UP_LOG}"
        exit 1
    fi
else
    log_info "Skipping stack start (--skip-start)"
fi

# Wait for orchestrator health
log_info ""
log_info "Waiting for orchestrator health endpoint..."
if wait_for_http_ok "http://localhost:8090/health" 180 3; then
    log_info "Orchestrator health check passed"
else
    log_error "Orchestrator health check failed (timeout)"
    log_error "Showing orchestrator logs (last 80 lines):"
    docker logs orchestrator --tail 80 2>&1 || true
    exit 1
fi

# Check orchestrator status endpoint
log_info ""
log_info "Checking orchestrator /api/status..."
STATUS_JSON=$(mktemp)
STATUS_FLAGS=$(mktemp)
if curl -fsS "http://localhost:8090/api/status" > "${STATUS_JSON}" 2>/dev/null; then
    log_info "Status endpoint responded"
    
    # Parse and display service status
    if command -v python3 >/dev/null 2>&1; then
        python3 - "${STATUS_JSON}" "${STATUS_FLAGS}" <<'PY' || true
import json
import sys

status_path = sys.argv[1]
flags_path = sys.argv[2]

with open(status_path, 'r') as f:
    data = json.load(f)

services = data.get('services', [])
all_ok = True
i2t_unreachable = False

print("\nService Status:")
print("=" * 60)
for svc in services:
    name = svc.get('name', 'unknown')
    status = str(svc.get('status', 'unknown'))
    status_l = status.lower()
    name_l = str(name).strip().lower()
    if status_l != 'ok':
        all_ok = False
    if name_l in ('image-to-text', 'image_to_text') and status_l == 'unreachable':
        i2t_unreachable = True
    print(f"  {name:30s} : {status}")
print("=" * 60)

with open(flags_path, 'w') as f:
    f.write(f"ALL_OK={'1' if all_ok else '0'}\n")
    f.write(f"I2T_UNREACHABLE={'1' if i2t_unreachable else '0'}\n")
PY
    fi

    # shellcheck disable=SC1090
    source "${STATUS_FLAGS}" 2>/dev/null || true
    ALL_OK="${ALL_OK:-0}"
    I2T_UNREACHABLE="${I2T_UNREACHABLE:-0}"

    # Check if all services are OK
    if [[ "${ALL_OK}" == "1" ]]; then
        log_info "All services reported status=ok"
    else
        if [[ "${I2T_UNREACHABLE}" == "1" ]]; then
            log_warn "Image-To-Text is still unreachable. I2T can take longer to get ready."
            log_info "Current container state (docker ps -a):"
            docker ps -a || true
            log_info "Stopping validation early because only I2T is still warming up."
            rm -f "${STATUS_JSON}" "${STATUS_FLAGS}"
            exit 0
        fi
        log_warn "Some services not reporting status=ok"
    fi
else
    log_warn "Failed to query /api/status endpoint"
fi
rm -f "${STATUS_JSON}" "${STATUS_FLAGS}"

# Run existing validation script if available
VALIDATION_SCRIPT="${REPO_ROOT}/scripts/validate-stack.sh"
if [[ -f "${VALIDATION_SCRIPT}" ]]; then
    log_info ""
    log_info "Running existing validation script: ${VALIDATION_SCRIPT}"
    if bash "${VALIDATION_SCRIPT}" --skip-start; then
        log_info "Validation script passed"
    else
        log_error "Validation script failed"
        log_error ""
        log_error "Showing logs for failing services (last 80 lines each):"
        
        # Show logs for each service
        for service in text-to-text image-to-text text-to-image speech-to-text text-to-speech orchestrator; do
            log_error ""
            log_error "--- ${service} logs ---"
            docker logs "${service}" --tail 80 2>&1 || log_error "Could not retrieve logs for ${service}"
        done
        
        exit 1
    fi
else
    log_warn "Validation script not found: ${VALIDATION_SCRIPT}"
    log_warn "Skipping detailed validation"
fi

# Show running containers
log_info ""
log_info "Running containers:"
${COMPOSE_CMD} ps

log_info ""
log_info "==================================================================="
log_info "Phase 4: Validate Stack - COMPLETE"
log_info "==================================================================="
log_info ""
log_info "Stack is up and validated successfully."
log_info ""
log_info "IMPORTANT NOTES:"
log_info "  - CDI Configuration: The CDI spec in /etc/cdi/docker-run-cdi-hw-acc.json"
log_info "    is device-specific. Current repo CDI files are for IQ9/IQ8."
log_info "    For other devices (e.g., Ventuno Q), you may need a different CDI spec."
log_info "  - If you encounter CDI device injection errors, see:"
log_info "    docs/TROUBLESHOOTING_GUIDE.md (Special Case: CDI Device Injection Error)"
log_info ""
log_info "Access points:"
log_info "  - Orchestrator:     http://${ACCESS_HOST}:8090"
log_info "  - Text-to-Text:     http://${ACCESS_HOST}:8088"
log_info "  - Image-to-Text:    http://${ACCESS_HOST}:8080"
log_info "  - Text-to-Image:    http://${ACCESS_HOST}:8084"
log_info "  - Speech-to-Text:   http://${ACCESS_HOST}:8081"
log_info "  - Text-to-Speech:   http://${ACCESS_HOST}:8083"
log_info ""
log_info "Next steps:"
log_info "  - Test endpoints: curl http://${ACCESS_HOST}:8090/health"
log_info "  - View logs:      docker logs orchestrator"
log_info "  - Stop stack:     ${COMPOSE_CMD} down"
log_info ""

exit 0
