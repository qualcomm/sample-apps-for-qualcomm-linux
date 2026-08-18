#!/bin/bash
# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# =============================================================================
# Phase 3: Build Stack
# Builds all service Docker images using existing build scripts where available.
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# shellcheck disable=SC1091
source "${REPO_ROOT}/scripts/lib/common.sh"

FORCE_MODE=0
FORCE_CLEAN=0
BUILD_SERVICES=()
STATE_DIR="${REPO_ROOT}/.genai-state"

#why are we using build.sh instead of docker build?
usage() {
    cat <<USAGE
Usage: $0 [OPTIONS] [SERVICE...]

Phase 3: Build Stack
- Build all service Docker images using direct docker build commands
- Build base images if they don't exist
- Use DOCKER_BUILDKIT for optimized builds

Options:
  --force         Force rebuild all images
  --force-clean   Clean Docker build cache and remove existing images before building
  --help          Show this help

Services (optional, default: all):
  text-to-text    Build only text-to-text service
  image-to-text   Build only image-to-text service
  text-to-image   Build only text-to-image service
  speech-to-text  Build only speech-to-text service
  text-to-speech  Build only text-to-speech service
  orchestrator    Build only orchestrator service
  all             Build all services (default)

Examples:
  # Build all services (including base images)
  $0

  # Build only text-to-text and orchestrator
  $0 text-to-text orchestrator

  # Force rebuild all services
  $0 --force

  # Clean cache, remove existing images, and rebuild
  $0 --force-clean
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force)
            FORCE_MODE=1
            shift
            ;;
        --force-clean)
            FORCE_CLEAN=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        text-to-text|image-to-text|text-to-image|speech-to-text|text-to-speech|orchestrator|all)
            BUILD_SERVICES+=("$1")
            shift
            ;;
        *)
            log_error "Unknown argument: $1"
            usage
            exit 1
            ;;
    esac
done

# Default to all services if none specified
if [[ ${#BUILD_SERVICES[@]} -eq 0 ]]; then
    BUILD_SERVICES=("all")
fi

# Expand "all" to individual services
if [[ " ${BUILD_SERVICES[*]} " =~ " all " ]]; then
    BUILD_SERVICES=(base-images text-to-text image-to-text text-to-image speech-to-text text-to-speech orchestrator)
fi

log_info "==================================================================="
log_info "Phase 3: Build Stack"
log_info "==================================================================="
log_info "Services to build: ${BUILD_SERVICES[*]}"

cd "${REPO_ROOT}"

COMPOSE_CMD=""
if [[ ${FORCE_CLEAN} -eq 1 ]]; then
    COMPOSE_CMD="$(detect_compose_cmd)"
fi

# Optional: Clean build cache and remove existing images
#for this command actually go through the first command in testing and validation
# that command includes removing the existing images and rebuilding from scratch
if [[ ${FORCE_CLEAN} -eq 1 ]]; then
    log_info "Cleaning Docker build cache and removing existing images (--force-clean)..."
    
    # Stop and remove containers
    ${COMPOSE_CMD} down --remove-orphans 2>/dev/null || true
    docker rm -f text-to-text image-to-text text-to-image speech-to-text text-to-speech orchestrator 2>/dev/null || true
    
    # Remove service images
    docker image rm -f text-to-text:latest image-to-text:latest image-to-text:responses-v1 \
                       text-to-image:latest speech-to-text:latest text-to-speech:latest \
                       orchestrator:latest 2>/dev/null || true
    
    # Clean build cache
    if docker builder prune -af; then
        log_info "Docker build cache cleaned and images removed"
    else
        log_warn "Failed to clean Docker build cache (continuing anyway)"
    fi
fi

FAILURES=0
declare -A BUILD_RESULTS

# Helper function to build a service using direct docker build
#why are we using build.sh instead of docker build?
build_service() {
    local service_name="$1"
    local service_dir="$2"
    local image_name="$3"
    
    log_info ""
    log_info "Building ${service_name}..."
    
    # Check if image already exists and skip if not forcing
    if [[ ${FORCE_MODE} -eq 0 ]] && docker image inspect "${image_name}" >/dev/null 2>&1; then
        log_info "  Image ${image_name} already exists (use --force to rebuild)"
        BUILD_RESULTS["${service_name}"]="SKIPPED (exists)"
        return 0
    fi
    
    # Build using direct docker build command (as per README)
    log_info "  Building ${image_name} from ${service_dir}"
    if DOCKER_BUILDKIT=1 docker build --progress=plain -t "${image_name}" "${service_dir}"; then
        log_info "  [PASS] ${service_name} built successfully"
        BUILD_RESULTS["${service_name}"]="PASS"
        return 0
    else
        log_error "  [FAIL] ${service_name} build failed"
        BUILD_RESULTS["${service_name}"]="FAIL"
        return 1
    fi
}

# Build services in dependency order
# Base images must be built first (ubuntu-runtime:24.04, genai-build-base:latest)
#why?? shouldn't those be added here??
#add

# Helper to check if service should be built
should_build() {
    local service="$1"
    for s in "${BUILD_SERVICES[@]}"; do
        if [[ "${s}" == "${service}" ]]; then
            return 0
        fi
    done
    return 1
}

has_qairt_sdk_layout() {
    local root="${REPO_ROOT}/qairt-sdk"
    [[ -d "${root}/include/Genie" ]] && [[ -d "${root}/include/QNN" ]] && [[ -d "${root}/lib" ]]
}

prepare_base_build_prereqs() {
    log_info ""
    log_info "Preparing shared base build prerequisites..."

    if has_qairt_sdk_layout; then
        log_info "  qairt-sdk layout OK"
    else
        log_info "  qairt-sdk missing/incomplete; running scripts/download-qairt-sdk.sh --service base"
        if bash "${REPO_ROOT}/scripts/download-qairt-sdk.sh" --service base; then
            if has_qairt_sdk_layout; then
                log_info "  [PASS] qairt-sdk prepared"
            else
                log_error "  [FAIL] qairt-sdk still incomplete after download-qairt-sdk.sh"
                return 1
            fi
        else
            log_error "  [FAIL] Failed to prepare qairt-sdk"
            return 1
        fi
    fi

    log_info "  Ensuring ubuntu:24.04 exists via scripts/pull-ubuntu-arm64.sh"
    if bash "${REPO_ROOT}/scripts/pull-ubuntu-arm64.sh"; then
        log_info "  [PASS] ubuntu:24.04 ready"
    else
        log_error "  [FAIL] Failed to prepare ubuntu:24.04"
        return 1
    fi
}

# Base images (ubuntu-runtime:24.04, genai-build-base:latest)
if should_build "base-images"; then
    log_info ""
    log_info "Building base images..."
    if ! prepare_base_build_prereqs; then
        BUILD_RESULTS["base-images"]="FAIL (prereqs)"
        FAILURES=$((FAILURES + 1))
    else
    
    # Build ubuntu-runtime:24.04
    if [[ ${FORCE_MODE} -eq 0 ]] && docker image inspect "ubuntu-runtime:24.04" >/dev/null 2>&1; then
        log_info "  Image ubuntu-runtime:24.04 already exists (use --force to rebuild)"
        BUILD_RESULTS["ubuntu-runtime"]="SKIPPED (exists)"
    else
        log_info "  Building ubuntu-runtime:24.04..."
        if DOCKER_BUILDKIT=1 docker build --progress=plain -f Dockerfile.runtime -t ubuntu-runtime:24.04 .; then
            log_info "  [PASS] ubuntu-runtime:24.04 built successfully"
            BUILD_RESULTS["ubuntu-runtime"]="PASS"
        else
            log_error "  [FAIL] ubuntu-runtime:24.04 build failed"
            BUILD_RESULTS["ubuntu-runtime"]="FAIL"
            FAILURES=$((FAILURES + 1))
        fi
    fi
    
    # Build genai-build-base:latest
    if [[ ${FORCE_MODE} -eq 0 ]] && docker image inspect "genai-build-base:latest" >/dev/null 2>&1; then
        log_info "  Image genai-build-base:latest already exists (use --force to rebuild)"
        BUILD_RESULTS["genai-build-base"]="SKIPPED (exists)"
    else
        log_info "  Building genai-build-base:latest..."
        if DOCKER_BUILDKIT=1 docker build --progress=plain -f Dockerfile.build-base -t genai-build-base:latest .; then
            log_info "  [PASS] genai-build-base:latest built successfully"
            BUILD_RESULTS["genai-build-base"]="PASS"
        else
            log_error "  [FAIL] genai-build-base:latest build failed"
            BUILD_RESULTS["genai-build-base"]="FAIL"
            FAILURES=$((FAILURES + 1))
        fi
    fi
    fi
fi

# Text-to-Text
if should_build "text-to-text"; then
    if ! build_service "text-to-text" "${REPO_ROOT}/core-services/text-to-text" "text-to-text:latest"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Image-to-Text
if should_build "image-to-text"; then
    if ! build_service "image-to-text" "${REPO_ROOT}/core-services/image-to-text" "image-to-text:responses-v1"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Text-to-Image
if should_build "text-to-image"; then
    if ! build_service "text-to-image" "${REPO_ROOT}/core-services/text-to-image" "text-to-image:latest"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Speech-to-Text
if should_build "speech-to-text"; then
    if ! build_service "speech-to-text" "${REPO_ROOT}/core-services/speech-to-text" "speech-to-text:latest"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Text-to-Speech
if should_build "text-to-speech"; then
    if ! build_service "text-to-speech" "${REPO_ROOT}/core-services/text-to-speech/meloTTS" "text-to-speech:latest"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Orchestrator (build directly, no build.sh)
if should_build "orchestrator"; then
    if ! build_service "orchestrator" "${REPO_ROOT}/core-services/orchestrator/" "orchestrator:latest"; then
        FAILURES=$((FAILURES + 1))
    fi
fi

# Summary
log_info ""
log_info "==================================================================="
log_info "Build Summary"
log_info "==================================================================="
for service in "${BUILD_SERVICES[@]}"; do
    printf "  %-20s : %s\n" "${service}" "${BUILD_RESULTS[${service}]:-NOT BUILT}"
done
log_info "==================================================================="

if [[ ${FAILURES} -eq 0 ]]; then
    log_info ""
    log_info "Phase 3: Build Stack - COMPLETE"
    log_info ""
    log_info "All images built successfully. Image list:"
    docker images --filter "reference=text-to-text:latest" \
                   --filter "reference=image-to-text:responses-v1" \
                   --filter "reference=text-to-image:latest" \
                   --filter "reference=speech-to-text:latest" \
                   --filter "reference=text-to-speech:latest" \
                   --filter "reference=orchestrator:latest" \
                   --format "  {{.Repository}}:{{.Tag}}  {{.Size}}  {{.CreatedAt}}"
    exit 0
else
    log_error ""
    log_error "Phase 3: Build Stack - FAILED"
    log_error ""
    log_error "Failed to build ${FAILURES} service(s). Check logs above for details."
    log_error ""
    log_error "NOTE: Base images (ubuntu-runtime:24.04, genai-build-base:latest) must be built first."
    log_error "      If base image builds failed, check:"
    log_error "        - Dockerfile.runtime exists in repo root"
    log_error "        - Dockerfile.build-base exists in repo root"
    log_error "        - qairt-sdk/ directory is present (run: bash scripts/download-qairt-sdk.sh --service base)"
    log_error ""
    log_error "For service-specific build failures, check:"
    log_error "        - Required SDK directories are present (whisper_sdk, melo_sdk)"
    log_error "        - Model directories exist under /opt/genai-studio-models/"
    exit 1
fi
