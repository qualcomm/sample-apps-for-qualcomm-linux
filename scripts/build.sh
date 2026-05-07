# Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause-Clear

#sdk_version="1.0"
# Function to display help menu
#!/bin/bash

# ==============================================================================
# SECTION 0: VARIABLES & SETUP HELPERS
# ==============================================================================
SDK_ROOT="${ESDK_ROOT:-$(pwd)}"
SYSROOT_DIR="${SDK_ROOT}/sysroots/armv8a-qcom-linux"
SYSROOT_INCDIR="${SYSROOT_DIR}/usr/include/gstreamer-1.0/gst/sampleapps"
SYSROOT_LIBDIR="${SYSROOT_DIR}/usr/lib"
INSTALL_INCDIR="${SDK_ROOT}/build/install/usr/include/gstreamer-1.0/gst/sampleapps"
INSTALL_LIBDIR="${SDK_ROOT}/build/install/usr/lib"
QIMSDK_TARGET_BIN_DIR="/usr/bin"
QIMSDK_TARGET_CONFIG_DIR="/etc"
QIMSDK_TARGET_MEDIA_DIR="/usr/share/media"

# List of C Sample Apps
SAMPLE_APPS=(
   # "gst-appsink-example"
    "gst-sample-apps-utils"
    "gst-camera-single-stream-example"
    "gst-multi-stream-example"
   # "gst-snapshot-stream-example"
   # "gst-add-remove-streams-runtime"
    "gst-multi-camera-example"
   # "gst-ai-classification"
   # "gst-ai-face-detection"
   # "gst-camera-single-stream-example"
   # "gst-video-playback-example"
   # "gst-video-transcode-example"
)

show_help() {
    echo "Usage: $0 [OPTION]"
    echo
    echo "Options:"
    echo "  -h, --help    Show this help message"
    echo "  -l, --list    List available sample apps"
    echo
    echo "To edit the sample app list:"
    echo "  - For C/C++ sample apps, edit SAMPLE_APPS in this script"
    echo "  - For Python sample apps, edit gst-plugins-imsdk/gst-python-examples/CMakeLists.txt"
    echo
    echo "Available sample apps:"
    for app in "${SAMPLE_APPS[@]}"; do
        echo "  - $app"
    done
}

case "$1" in
    -h|--help)
        show_help
        exit 0
        ;;
    -l|--list)
        for app in "${SAMPLE_APPS[@]}"; do
            echo "$app"
        done
        exit 0
        ;;
esac

function qimsdk-print-setup() {
    echo "============================================================"
    echo " Build setup successfully loaded!"
    echo "============================================================"
    echo "Available commands:"
    echo "  qimsdk-build-sample-apps <path-to-gst-plugins-imsdk-repo>"
    echo
    echo "Install location on target:"
    echo "  Binaries : ${QIMSDK_TARGET_BIN_DIR}"
    echo
    echo "Host staging location after install:"
    echo "  $(pwd)/build/install${QIMSDK_TARGET_BIN_DIR}"
    echo "============================================================"
}

function qimsdk-print-build-success() {
    local TARGET_NAME="${1}"
    echo "============================================================"
    echo " SUCCESS: ${TARGET_NAME} built and installed successfully"
    echo " Target binary location : ${QIMSDK_TARGET_BIN_DIR}"
    echo " Host staged location   : $(pwd)/build/install${QIMSDK_TARGET_BIN_DIR}"
    echo "============================================================"
}

# ==============================================================================
# SECTION 1: CORE CMAKE BUILD ENGINE
# ==============================================================================

function qimsdk-cmake-configure() {
    local SOURCE_PATH="${1}"
    local TARGET="${2}"
    local ROOT_PATH=$(pwd)

    [ ! -d "${SOURCE_PATH}" ] && { echo "ERROR: Source path not found: ${SOURCE_PATH}"; return 1; }

    # Shift past the first two arguments to capture any extra flags
    shift 2
    local CMAKE_CUSTOM_CONFIG_FLAGS=("$@")

    (
        export CFLAGS="-mbranch-protection=standard -fstack-protector-strong -O2 -D_FORTIFY_SOURCE=2 -Wformat -Wformat-security -Werror=format-security -pipe ${EXTRA_CFLAGS:-}"
        export CXXFLAGS="${CFLAGS} ${EXTRA_CXXFLAGS:-}"

        local CMAKE_FLAGS=(
            "-DCMAKE_VERBOSE_MAKEFILE:BOOL=ON"
            "-DCMAKE_BUILD_TYPE=Debug"
            "-DCMAKE_INSTALL_PREFIX=/usr"
            "-DGST_PLUGINS_QTI_OSS_INSTALL_BINDIR=bin"
            "-DGST_PLUGINS_QTI_OSS_INSTALL_CONFIG=/etc"
            "-DGST_PLUGINS_QTI_OSS_INSTALL_MEDIA=/usr/share/media"
            "-DGST_VERSION_REQUIRED=1.22"
            "-DGST_PLUGINS_QTI_OSS_VERSION=1.22"
            "-DGST_PLUGINS_QTI_OSS_INSTALL_LIBDIR=lib"
            "-DGST_PLUGINS_QTI_OSS_INSTALL_INCDIR=include"
            "${CMAKE_CUSTOM_CONFIG_FLAGS[@]}"
        )

        mkdir -p "${ROOT_PATH}/build/sources/${TARGET}"
        cd "${ROOT_PATH}/build/sources/${TARGET}" || exit 1

        echo "--- Configuring ${TARGET} ---"
        cmake "${CMAKE_FLAGS[@]}" "${SOURCE_PATH}"
    ) || return 1
}

function qimsdk-cmake-compile() {
    local TARGET="${1}"
    [ ! -d "build/sources/${TARGET}" ] && { echo "ERROR: Build directory not found for ${TARGET}"; return 1; }

    echo "--- Compiling ${TARGET} ---"
    cmake --build "build/sources/${TARGET}" || return 1
}

function qimsdk-cmake-install() {
    local TARGET="${1}"
    [ ! -d "build/sources/${TARGET}" ] && { echo "ERROR: Build directory not found for ${TARGET}"; return 1; }

    echo "--- Installing ${TARGET} ---"
    DESTDIR="$(pwd)/build/install" cmake --install "build/sources/${TARGET}" || return 1
}

function qimsdk-cmake-build() {
    local SOURCE_PATH="${1}"
    local TARGET=$(basename "${SOURCE_PATH}")
    shift 1

    qimsdk-cmake-configure "${SOURCE_PATH}" "${TARGET}" "$@" && \
    qimsdk-cmake-compile "${TARGET}" && \
    qimsdk-cmake-install "${TARGET}" && \
    qimsdk-print-build-success "${TARGET}"
}

# ==============================================================================
# SECTION 2: APP SPECIFIC BUILDERS
# ==============================================================================

function qimsdk-build-python-examples() {
    local REPO_PATH="${1}"
    local SOURCE_DIR="${REPO_PATH}/gst-python-examples"

    [ ! -d "${SOURCE_DIR}" ] && { echo "ERROR: Python examples dir not found"; return 1; }

    qimsdk-cmake-build "${SOURCE_DIR}"
}

function qimsdk-build-sample-apps() {
    local REPO_PATH="${1}"

    if [ -z "${REPO_PATH}" ]; then
        echo "Usage: qimsdk-build-sample-apps <path-to-gst-plugins-imsdk-repo>"
        return 1
    fi

    # Ensure path is absolute
    REPO_PATH=$(realpath "${REPO_PATH}")

    echo "Select build option:"
    echo "1) Build C Sample Apps"
    echo "2) Build Python Examples"
    read -p "Enter choice [1-2]: " choice

    case $choice in
        1)
            # Build C Sample Apps from the array
            for APP in "${SAMPLE_APPS[@]}"; do
                if [ "${APP}" = "gst-sample-apps-utils" ]; then
                qimsdk-cmake-build "${REPO_PATH}/gst-sample-apps/${APP}" || return 1

                mkdir -p "${SYSROOT_INCDIR}" "${SYSROOT_LIBDIR}" || return 1

                cp -vf \
                    "${INSTALL_INCDIR}/gst_sample_apps_utils.h" \
                    "${SYSROOT_INCDIR}/" || return 1

                cp -av \
                    "${INSTALL_LIBDIR}/libgstappsutils.so"* \
                    "${SYSROOT_LIBDIR}/" || return 1
                else
		    EXTRA_CFLAGS="-I${SYSROOT_INCDIR}" \
                    EXTRA_CXXFLAGS="-I${SYSROOT_INCDIR}" \
                        qimsdk-cmake-build "${REPO_PATH}/gst-sample-apps/${APP}" || return 1
                fi
            done

            echo "============================================================"
            echo " All C sample apps compiled successfully"
            echo " Binaries available on target under: ${QIMSDK_TARGET_BIN_DIR}"
            echo " Host staged files under          : $(pwd)/build/install${QIMSDK_TARGET_BIN_DIR}"
            echo "============================================================"
            ;;
        2)
            # Build Python Examples
            qimsdk-build-python-examples "${REPO_PATH}" || return 1

            echo "============================================================"
            echo " Python sample apps compiled successfully"
            echo " Scripts available on target under: ${QIMSDK_TARGET_BIN_DIR}"
            echo " Host staged files under         : $(pwd)/build/install${QIMSDK_TARGET_BIN_DIR}"
            echo "============================================================"
            ;;
        *)
            echo "Invalid choice."
            return 1
            ;;
    esac
}

# Execute setup message upon sourcing
qimsdk-print-setup
