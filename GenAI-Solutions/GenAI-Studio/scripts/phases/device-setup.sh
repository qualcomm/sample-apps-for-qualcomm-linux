#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# Phase 1: Device Setup (Enhanced with Provisioning)
# Detects device profile, provisions missing components, validates prerequisites.
#all changes suggested in this file are Phase 1 changes
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091 #what exactly is this?
source "${REPO_ROOT}/scripts/lib/common.sh"

FORCE_MODE=0
PROVISION_MODE=1  # Default to provision mode
VALIDATE_ONLY=0
STATE_DIR="${REPO_ROOT}/.genai-state"

usage() {
    cat <<USAGE
Usage: $0 [OPTIONS]

Phase 1: Device Setup
- Detect device profile (Ubuntu 24.04 / QLI 1.x / QLI 2.0)
- Provision missing components (packages, Docker, CDI, QAIRT)
- Validate prerequisites
- Generate/update .env with detected defaults

Options:
  --provision        Enable provisioning mode (install missing components) [DEFAULT]
  --validate-only    Only validate, do not provision or modify system
  --force            Overwrite existing .env values and force reinstalls
  --help             Show this help

Modes:
  Default:           Provision mode (validate + install missing components)
  --validate-only:   Only validate, exit with error if incomplete

Examples:
  # Provision missing components (default)
  ./scripts/phases/device-setup.sh

  # Strict validation only (fail if incomplete)
  ./scripts/phases/device-setup.sh --validate-only

  # Force reinstall/overwrite
  ./scripts/phases/device-setup.sh --force

USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --provision)
            PROVISION_MODE=1
            VALIDATE_ONLY=0
            shift
            ;;
        --validate-only)
            VALIDATE_ONLY=1
            PROVISION_MODE=0
            shift
            ;;
        --force)
            FORCE_MODE=1
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
log_info "Phase 1: Device Setup"
log_info "==================================================================="
log_info "Mode: $(if [[ ${VALIDATE_ONLY} -eq 1 ]]; then echo 'VALIDATE-ONLY'; else echo 'PROVISION'; fi)"
log_info "==================================================================="

# =============================================================================
# Profile Detection
# =============================================================================
log_info "Detecting device profile..."
PROFILE="$(detect_profile)"

if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    source /etc/os-release
    log_info "OS: ${NAME:-unknown} ${VERSION:-unknown}"
    log_info "ID: ${ID:-unknown}"
    log_info "VERSION_ID: ${VERSION_ID:-unknown}"
else
    log_warn "/etc/os-release not found"
fi

UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"
UNAME_R="$(uname -r)"
log_info "Kernel: ${UNAME_S} ${UNAME_R}"
log_info "Architecture: ${UNAME_M}"
log_info "Detected Profile: ${PROFILE}"

if [[ "${PROFILE}" == "unknown" ]]; then
    log_warn "Could not detect profile (Ubuntu/QLI 1.x/QLI 2.0)"
    log_warn "Provisioning may not work correctly"
fi


# =============================================================================
# Ubuntu 24.04 specific: Install Qualcomm packages
# =============================================================================
if [[ "${PROFILE}" == "ubuntu" ]] && [[ ${PROVISION_MODE} -eq 1 ]]; then
    log_info "Checking Qualcomm runtime packages (Ubuntu 24.04)..."
    
    MISSING_PKGS=()
    for pkg in qcom-fastrpc1 qcom-libdmabufheap-dev qcom-fastrpc-dev qcom-dspservices-headers-dev libqnn1 qnn-tools libsnpe1 snpe-tools; do
        if ! dpkg -l | grep -q "^ii  ${pkg}"; then
            MISSING_PKGS+=("${pkg}")
        fi
    done
    
    if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
        log_info "Installing missing Qualcomm packages: ${MISSING_PKGS[*]}"
        
        # Add Qualcomm PPA if not present
        if ! grep -Rqs 'ubuntu-qcom-iot/qcom-ppa' /etc/apt/sources.list /etc/apt/sources.list.d 2>/dev/null; then
            log_info "Adding Qualcomm PPA..."
            sudo add-apt-repository -y ppa:ubuntu-qcom-iot/qcom-ppa
        fi
        
        sudo apt-get update
        sudo apt-get install -y "${MISSING_PKGS[@]}"
        log_info "Qualcomm packages installed"
    else
        log_info "All Qualcomm packages already installed"
    fi
fi

#figure out what to do to automate the docker thingy cus it doesn't make sense to go to a new shell?
#like when will this add up??
# =============================================================================
# Docker validation/provisioning
# =============================================================================
log_info "Checking Docker installation..."
if ! command -v docker >/dev/null 2>&1; then
    log_warn "Docker not found"
    
    if [[ ${PROVISION_MODE} -eq 1 ]]; then
        log_info "Provisioning Docker for profile: ${PROFILE}"
        
        case "${PROFILE}" in
            ubuntu)
                log_info "Installing Docker (Ubuntu 24.04)..."
                sudo apt-get remove -y docker.io docker-doc docker-compose podman-docker containerd runc 2>/dev/null || true
                sudo install -m 0755 -d /etc/apt/keyrings
                curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
                sudo chmod a+r /etc/apt/keyrings/docker.gpg
                echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
                  sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
                sudo apt-get update
                sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin docker-compose
                sudo systemctl enable --now docker
                sudo usermod -aG docker "$USER"
                log_info "Docker installed successfully."
                log_info "Docker installed successfully."
                log_warn ""
                log_warn "IMPORTANT: Docker group membership requires session refresh."
                log_warn "Checking if docker is accessible without sudo..."
                log_warn ""
                #figure out what to do to automate the docker thingy cus it doesn't make sense to go to a new shell?
                #like when will this add up??
                if docker ps >/dev/null 2>&1; then
                    log_info "Docker access verified (no sudo required)"
                else
                    log_error ""
                    log_error "Docker group not active in current session."
                    log_error "Please run ONE of the following to activate docker group:"
                    log_error "  1. newgrp docker  # Activate in new shell"
                    log_error "  2. Log out and log back in"
                    log_error ""
                    log_error "After activating docker group, resume with:"
                    log_error "  ./scripts/genai-studio.sh --resume"
                    log_error ""
                    exit 2
                fi
                ;;
            qli1|qli2)
                log_warn "Docker not found on QLI. Docker should be pre-installed."
                log_error "Please verify QLI image includes Docker or install manually."
                exit 1
                ;;
            *)
                log_error "Cannot provision Docker for unknown profile"
                exit 1
                ;;
        esac
    else
        log_error "Docker not found. Run with --provision to install, or see: docs/setup/DEVICE_SETUP.md"
        if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
            exit 1
        fi
    fi
fi

if command -v docker >/dev/null 2>&1; then
    DOCKER_VERSION="$(docker --version 2>/dev/null || echo 'unknown')"
    log_info "Docker: ${DOCKER_VERSION}"
fi

# =============================================================================
# Docker Compose validation
# =============================================================================
log_info "Checking Docker Compose..."
COMPOSE_CMD=""
if docker compose version >/dev/null 2>&1; then
    COMPOSE_CMD="docker compose"
    COMPOSE_VERSION="$(docker compose version 2>/dev/null || echo 'unknown')"
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE_CMD="docker-compose"
    COMPOSE_VERSION="$(docker-compose --version 2>/dev/null || echo 'unknown')"
else
    log_warn "Neither 'docker compose' nor 'docker-compose' found"
    if [[ ${PROVISION_MODE} -eq 1 ]] && [[ "${PROFILE}" == "ubuntu" ]]; then
        log_info "Docker Compose should have been installed with Docker"
    fi
    if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
        log_error "Docker Compose required but not found"
        exit 1
    fi
fi

if [[ -n "${COMPOSE_CMD}" ]]; then
    log_info "Compose: ${COMPOSE_VERSION}"
fi

# Check buildx
if docker buildx version >/dev/null 2>&1; then
    BUILDX_VERSION="$(docker buildx version 2>/dev/null || echo 'unknown')"
    log_info "Buildx: ${BUILDX_VERSION}"
else
    log_warn "docker buildx not available (may be needed for builds)"
    if [[ "${PROFILE}" =~ ^(qli1|qli2)$ ]] && [[ ${PROVISION_MODE} -eq 1 ]]; then
        log_info "Installing docker buildx for QLI..."
        mkdir -p ~/.docker/cli-plugins
        curl -L https://github.com/docker/buildx/releases/download/v0.14.1/buildx-v0.14.1.linux-arm64 \
          -o ~/.docker/cli-plugins/docker-buildx
        chmod +x ~/.docker/cli-plugins/docker-buildx
        if docker buildx version >/dev/null 2>&1; then
            log_info "docker buildx installed successfully"
        else
            log_warn "docker buildx installation may have failed"
        fi
    fi
fi

# =============================================================================
# CDI (Container Device Interface) setup
# =============================================================================
log_info "Checking CDI configuration..."

if [[ ! -d /etc/cdi ]]; then
    log_warn "/etc/cdi does not exist"
    if [[ ${PROVISION_MODE} -eq 1 ]]; then
        log_info "Creating /etc/cdi directory..."
        if sudo install -d /etc/cdi 2>/dev/null || install -d /etc/cdi 2>/dev/null; then
            log_info "Created /etc/cdi"
        else
            log_error "Could not create /etc/cdi (permission denied)"
            exit 1
        fi
    else
        log_warn "Run with --provision to create /etc/cdi"
        if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
            exit 1
        fi
    fi
fi

if [[ -d /etc/cdi ]]; then
    log_info "/etc/cdi exists"
    
    if [[ ! -f /etc/cdi/docker-run-cdi-hw-acc.json ]]; then
        log_warn "CDI spec not found: /etc/cdi/docker-run-cdi-hw-acc.json"
        
        if [[ ${PROVISION_MODE} -eq 1 ]]; then
            CDI_SOURCE="$(get_cdi_source_path "${PROFILE}" "${REPO_ROOT}")"
            
            if [[ -n "${CDI_SOURCE}" ]] && [[ -f "${CDI_SOURCE}" ]]; then
                log_info "Installing CDI spec from: ${CDI_SOURCE}"
                if sudo cp "${CDI_SOURCE}" /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null || \
                   cp "${CDI_SOURCE}" /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null; then
                    sudo chmod 644 /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null || \
                      chmod 644 /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null
                    log_info "CDI spec installed successfully"
                    
                    # Enable CDI in Docker daemon for Ubuntu
                    if [[ "${PROFILE}" == "ubuntu" ]]; then
                        log_info "Enabling CDI in Docker daemon..."
                        sudo install -m 0755 -d /etc/docker
                        printf '{\n  "features": {\n    "cdi": true\n  }\n}\n' | sudo tee /etc/docker/daemon.json >/dev/null
                        sudo systemctl restart docker
                        log_info "Docker daemon restarted with CDI enabled"
                    fi
                else
                    log_error "Failed to install CDI spec (permission denied)"
                    exit 1
                fi
            else
                log_error "CDI source not found: ${CDI_SOURCE}"
                exit 1
            fi
        else
            log_warn "Run with --provision to install CDI spec"
            if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
                exit 1
            fi
        fi
    else
        log_info "CDI spec found: /etc/cdi/docker-run-cdi-hw-acc.json"
        
        # Verify CDI spec contains expected device
        if grep -q 'qualcomm.com/device=cdi-hw-acc' /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null; then
            log_info "CDI spec validated (contains cdi-hw-acc device)"
            
            # Check for problematic /dev/kgsl-3d0 device
            if grep -q '"/dev/kgsl-3d0"' /etc/cdi/docker-run-cdi-hw-acc.json 2>/dev/null; then
                log_warn "CDI spec contains /dev/kgsl-3d0 device node"
                log_warn "This device may not exist on all platforms and can cause CDI injection errors"
                log_warn "If you encounter 'failed to stat CDI host device "/dev/kgsl-3d0"' errors:"
                log_warn "  Remove the /dev/kgsl-3d0 entry from /etc/cdi/docker-run-cdi-hw-acc.json"
                log_warn "  See: docs/TROUBLESHOOTING_GUIDE.md (Special Case: CDI Device Injection Error)"
            fi
        else
            log_warn "CDI spec may be invalid or corrupted"
        fi
    fi
fi



# =============================================================================
# QAIRT installation and flat lib bundle
# =============================================================================
log_info "Checking QAIRT installation..."
QAIRT_DEFAULT_PATH="/opt/qairt/current/qairt_245_flat_libs"

if [[ ! -d /opt/qairt/current ]]; then
    log_warn "QAIRT not found at /opt/qairt/current"
    
    if [[ ${PROVISION_MODE} -eq 1 ]]; then
        log_info "Provisioning QAIRT..."
        
        # Create standard runtime layout (profile-aware sudo usage)
        log_info "Creating standard runtime layout..."
        
        # Ubuntu uses sudo, QLI does not
        if [[ "${PROFILE}" == "ubuntu" ]]; then
            sudo mkdir -p /opt/genai-studio-models/{text-to-text,speech-to-text,text-to-speech,image-to-text,text-to-image}
            sudo mkdir -p /opt/genai-studio-cache/huggingface
            sudo mkdir -p /opt/qairt
            sudo chown -R "$USER":"$USER" /opt/genai-studio-models /opt/genai-studio-cache /opt/qairt
            log_info "Created /opt directories with sudo (Ubuntu)"
        else
            mkdir -p /opt/genai-studio-models/{text-to-text,speech-to-text,text-to-speech,image-to-text,text-to-image}
            mkdir -p /opt/genai-studio-cache/huggingface
            mkdir -p /opt/qairt
            chown -R "$USER":"$USER" /opt/genai-studio-models /opt/genai-studio-cache /opt/qairt 2>/dev/null || true
            log_info "Created /opt directories (QLI)"
        fi
        
        # Download and install QAIRT
        log_info "Downloading QAIRT SDK..."
        load_versions_manifest "${REPO_ROOT}"
        QAIRT_VER="${QAIRT_VERSION:-2.45.0.260326}"
        QAIRT_ZIP="/tmp/v${QAIRT_VER}.zip"
        QAIRT_URL="https://softwarecenter.qualcomm.com/api/download/software/sdks/Qualcomm_AI_Runtime_Community/All/${QAIRT_VER}/v${QAIRT_VER}.zip"
        
        log_info "Downloading QAIRT ${QAIRT_VER}..."
        if curl -fL "${QAIRT_URL}" -o "${QAIRT_ZIP}"; then
            log_info "Download complete"
            
            TMP_UNZIP="$(mktemp -d)"
            log_info "Extracting QAIRT..."
            unzip -q "${QAIRT_ZIP}" -d "${TMP_UNZIP}"
            
            # Profile-aware installation
            if [[ "${PROFILE}" == "ubuntu" ]]; then
                sudo mkdir -p "/opt/qairt/${QAIRT_VER}"
                if [[ -d "${TMP_UNZIP}/qairt/${QAIRT_VER}" ]]; then
                    sudo rsync -a "${TMP_UNZIP}/qairt/${QAIRT_VER}/" "/opt/qairt/${QAIRT_VER}/"
                else
                    sudo rsync -a "${TMP_UNZIP}/" "/opt/qairt/${QAIRT_VER}/"
                fi
                sudo ln -sfn "/opt/qairt/${QAIRT_VER}" /opt/qairt/current
                sudo ln -sfn /opt/qairt/current/bin /opt/qairt/bin
                sudo ln -sfn /opt/qairt/current/include /opt/qairt/include
                sudo ln -sfn /opt/qairt/current/lib /opt/qairt/lib
            else
                mkdir -p "/opt/qairt/${QAIRT_VER}"
                if [[ -d "${TMP_UNZIP}/qairt/${QAIRT_VER}" ]]; then
                    rsync -a "${TMP_UNZIP}/qairt/${QAIRT_VER}/" "/opt/qairt/${QAIRT_VER}/"
                else
                    rsync -a "${TMP_UNZIP}/" "/opt/qairt/${QAIRT_VER}/"
                fi
                ln -sfn "/opt/qairt/${QAIRT_VER}" /opt/qairt/current
                ln -sfn /opt/qairt/current/bin /opt/qairt/bin
                ln -sfn /opt/qairt/current/include /opt/qairt/include
                ln -sfn /opt/qairt/current/lib /opt/qairt/lib
            fi
            
            rm -rf "${TMP_UNZIP}"
            log_info "Created /opt/qairt/current symlink"
            
            log_info "QAIRT ${QAIRT_VER} installed successfully"
        else
            log_error "Failed to download QAIRT from ${QAIRT_URL}"
            log_error "Please download manually and extract to /opt/qairt/${QAIRT_VER}"
            exit 1
        fi
    else
        log_warn "Run with --provision to install QAIRT"
        if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
            exit 1
        fi
    fi
fi

if [[ -d /opt/qairt/current ]]; then
    log_info "QAIRT found at /opt/qairt/current"
    
    # Check/build flat lib bundle
    if [[ ! -d "${QAIRT_DEFAULT_PATH}" ]]; then
        log_warn "QAIRT flat libs not found at: ${QAIRT_DEFAULT_PATH}"
        
        if [[ ${PROVISION_MODE} -eq 1 ]]; then
            log_info "Building QAIRT flat lib bundle..."
            
            rm -rf "${QAIRT_DEFAULT_PATH}"
            mkdir -p "${QAIRT_DEFAULT_PATH}"
            
            # Copy aarch64 libs
            if [[ -d /opt/qairt/current/lib/aarch64-oe-linux-gcc11.2 ]]; then
                cp -a /opt/qairt/current/lib/aarch64-oe-linux-gcc11.2/*.so* "${QAIRT_DEFAULT_PATH}/"
                log_info "Copied aarch64 libs"
            else
                log_warn "aarch64 libs not found in QAIRT"
            fi
            
            # Copy hexagon v73 libs (selective copy to avoid ELF class issues)
            if [[ -d /opt/qairt/current/lib/hexagon-v73/unsigned ]]; then
                cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libQnnHtpV73*.so "${QAIRT_DEFAULT_PATH}/" 2>/dev/null || true
                cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libqnnhtpv73.cat "${QAIRT_DEFAULT_PATH}/" 2>/dev/null || true
                cp -a /opt/qairt/current/lib/hexagon-v73/unsigned/libsnpehtpv73.cat "${QAIRT_DEFAULT_PATH}/" 2>/dev/null || true
                log_info "Copied hexagon v73 libs"
            else
                log_warn "hexagon v73 libs not found in QAIRT"
            fi
            
            log_info "QAIRT flat lib bundle created: ${QAIRT_DEFAULT_PATH}"
        else
            log_warn "Run with --provision to build flat lib bundle"
            if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
                exit 1
            fi
        fi
    else
        log_info "QAIRT flat libs found: ${QAIRT_DEFAULT_PATH}"
    fi
fi


# =============================================================================
# FastRPC device nodes check
# =============================================================================
log_info "Checking FastRPC device nodes..."
declare -a FASTRPC_NODES=(
    "/dev/fastrpc-cdsp"
    "/dev/fastrpc-cdsp-secure"
    "/dev/fastrpc-cdsp1"
    "/dev/fastrpc-cdsp1-secure"
    "/dev/fastrpc-adsp-secure"
)

FASTRPC_MISSING=0
for node in "${FASTRPC_NODES[@]}"; do
    if [[ -c "${node}" ]]; then
        log_info "FastRPC node OK: ${node}"
    else
        log_warn "FastRPC node MISSING: ${node}"
        FASTRPC_MISSING=$((FASTRPC_MISSING + 1))
    fi
done

if [[ ${FASTRPC_MISSING} -gt 0 ]]; then
    log_warn "WARNING: ${FASTRPC_MISSING} FastRPC device node(s) missing"
    log_warn "This is expected if device setup is incomplete."
    log_warn "Services may fail at runtime without these nodes."
fi




# =============================================================================
# Detect FAST_RPC_SHELL_PATH (profile-aware) 
# =============================================================================
log_info "Detecting FastRPC shell path..."
FAST_RPC_SHELL_PATH=""
SEARCH_PATHS="$(get_fastrpc_shell_search_paths "${PROFILE}")"

for d in ${SEARCH_PATHS}; do
    if [[ -f "${d}/fastrpc_shell_unsigned_3" ]]; then
        FAST_RPC_SHELL_PATH="${d}/fastrpc_shell_unsigned_3"
        log_info "FastRPC shell found: ${FAST_RPC_SHELL_PATH}"
        break
    fi
done

if [[ -z "${FAST_RPC_SHELL_PATH}" ]]; then
    log_warn "Could not auto-detect FAST_RPC_SHELL_PATH"
    # Profile-specific defaults
    case "${PROFILE}" in
        qli2)
            FAST_RPC_SHELL_PATH="/usr/share/qcom/sa8775p/Qualcomm/IQ9075-EVK/dsp/cdsp/fastrpc_shell_unsigned_3"
            ;;
        *)
            FAST_RPC_SHELL_PATH="/usr/lib/dsp/cdsp/fastrpc_shell_unsigned_3"
            ;;
    esac
    log_warn "Using default: ${FAST_RPC_SHELL_PATH}"
fi

# =============================================================================
# Generate/update .env (only FAST_RPC_SHELL_PATH is required)
# =============================================================================
ENV_FILE="${REPO_ROOT}/.env"
log_info "Updating environment configuration: ${ENV_FILE}"

# Read existing .env if present
declare -A EXISTING_ENV
if [[ -f "${ENV_FILE}" ]]; then
    while IFS='=' read -r key value; do
        # Skip comments and empty lines
        [[ "${key}" =~ ^#.*$ ]] && continue
        [[ -z "${key}" ]] && continue
        EXISTING_ENV["${key}"]="${value}"
    done < "${ENV_FILE}"
fi

# Helper to set env var only if not already set (unless --force)
set_env_var() {
    local key="$1"
    local value="$2"
    
    if [[ ${FORCE_MODE} -eq 1 ]] || [[ -z "${EXISTING_ENV[${key}]:-}" ]]; then
        echo "${key}=${value}"
        log_info "Set ${key}=${value}"
    else
        echo "${key}=${EXISTING_ENV[${key}]}"
        log_info "Kept existing ${key}=${EXISTING_ENV[${key}]}"
    fi
}

# Build new .env content
{
    echo "# GenAI Studio Environment Configuration"
    echo "# Generated by scripts/phases/device-setup.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "# Profile: ${PROFILE}"
    echo ""
    
    # FAST_RPC_SHELL_PATH is required for QLI 1.x and Ubuntu
    set_env_var "FAST_RPC_SHELL_PATH" "${FAST_RPC_SHELL_PATH}"
    
    # Preserve any other existing vars not managed by this script
    for key in "${!EXISTING_ENV[@]}"; do
        case "${key}" in
            FAST_RPC_SHELL_PATH)
                # Already handled above
                ;;
            *)
                echo "${key}=${EXISTING_ENV[${key}]}"
                ;;
        esac
    done
} > "${ENV_FILE}.tmp"

mv "${ENV_FILE}.tmp" "${ENV_FILE}"
log_info "Environment configuration updated: ${ENV_FILE}"

# =============================================================================
# Optional: Run DSP validator
# =============================================================================
if [[ ${PROVISION_MODE} -eq 1 ]] && command -v qnn-platform-validator >/dev/null 2>&1; then
    log_info "Running DSP runtime validator..."
    if qnn-platform-validator --backend dsp --testBackend 2>&1 | tee /tmp/qnn-validator.log; then
        log_info "DSP validator: PASS"
    else
        log_warn "DSP validator: FAIL (this may be expected if DSP is not fully configured)"
        log_warn "See: /tmp/qnn-validator.log"
    fi
fi

# =============================================================================
# Final Summary
# =============================================================================
log_info "==================================================================="
log_info "Phase 1: Device Setup - COMPLETE"
log_info "==================================================================="
log_info ""
log_info "Summary:"
log_info "  - Profile: ${PROFILE}"
log_info "  - Docker: ${DOCKER_VERSION:-NOT FOUND}"
log_info "  - Compose: ${COMPOSE_VERSION:-NOT FOUND}"
log_info "  - Buildx: ${BUILDX_VERSION:-NOT FOUND}"
log_info "  - CDI: $(if [[ -f /etc/cdi/docker-run-cdi-hw-acc.json ]]; then echo 'configured'; else echo 'NOT CONFIGURED'; fi)"
log_info "  - QAIRT: $(if [[ -d /opt/qairt/current ]]; then echo 'installed'; else echo 'NOT INSTALLED'; fi)"
log_info "  - QAIRT flat libs: $(if [[ -d ${QAIRT_DEFAULT_PATH} ]]; then echo 'built'; else echo 'NOT BUILT'; fi)"
log_info "  - FastRPC nodes: $((${#FASTRPC_NODES[@]} - FASTRPC_MISSING))/${#FASTRPC_NODES[@]} present"
log_info "  - Environment: ${ENV_FILE}"
log_info ""

HAS_ERRORS=0

if [[ ${FASTRPC_MISSING} -gt 0 ]]; then
    log_warn "WARNING: ${FASTRPC_MISSING} FastRPC device node(s) missing"
    log_warn "Services may fail at runtime without these nodes"
    HAS_ERRORS=1
fi

if [[ ! -d /opt/qairt/current ]]; then
    log_warn "WARNING: QAIRT not installed"
    log_warn "Run with --provision to install QAIRT"
    HAS_ERRORS=1
fi

if [[ ! -d "${QAIRT_DEFAULT_PATH}" ]]; then
    log_warn "WARNING: QAIRT flat lib bundle not built"
    log_warn "Run with --provision to build flat lib bundle"
    HAS_ERRORS=1
fi

if [[ ! -f /etc/cdi/docker-run-cdi-hw-acc.json ]]; then
    log_warn "WARNING: CDI spec not installed"
    log_warn "Run with --provision to install CDI spec"
    HAS_ERRORS=1
fi

if [[ ${HAS_ERRORS} -eq 1 ]]; then
    log_warn ""
    log_warn "Device setup incomplete. Review warnings above."
    log_warn "To provision missing components: ./scripts/phases/device-setup.sh --provision"
    log_warn "See: docs/setup/DEVICE_SETUP.md"
    log_warn ""
    
    if [[ ${VALIDATE_ONLY} -eq 1 ]]; then
        log_error "Validation failed (--validate-only mode)"
        exit 1
    fi
fi

if [[ ${PROVISION_MODE} -eq 1 ]] && [[ ${HAS_ERRORS} -eq 0 ]]; then
    log_info "Device provisioning complete and validated!"
    log_info "Next: Run model setup phase"
fi

exit 0
