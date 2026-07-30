#==============================================================================
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#==============================================================================

cmake_minimum_required(VERSION 3.18.2)
project(asr-service)

set(APP "asr-service")

# C++17 required for std::filesystem, structured bindings, etc.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")

add_compile_options(-O2 -Wall -fPIC)

# ------------------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------------------
add_executable(${APP}
    main.cpp
    AsrConfig.cpp
    AsrService.cpp
    ErrorResponder.cpp
    HttpPolicies.cpp
    LegacyRoutes.cpp
    ModelCatalog.cpp
    OpenAiRoutes.cpp
    RealtimeSessionStore.cpp
    RealtimeSessionHandlers.cpp
    RealtimeRoutes.cpp
    TranscriptionExecutor.cpp
    WhisperEngine.cpp
)

# ------------------------------------------------------------------------------
# Whisper SDK
#
# WHISPER_SDK_ROOT can be overridden at configure time:
#   cmake -DWHISPER_SDK_ROOT=/path/to/whisper-sdk ..
# ------------------------------------------------------------------------------
set(WHISPER_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../whisper_sdk"
    CACHE PATH "Path to the Whisper SDK root (contains include/ and libs/)")

if(EXISTS "${WHISPER_SDK_ROOT}/include/cpp/Whisper.h")
    set(WHISPER_INCLUDE_DIR "${WHISPER_SDK_ROOT}/include/cpp")
elseif(EXISTS "${WHISPER_SDK_ROOT}/include/npu/rpc/linux/Whisper.h")
    set(WHISPER_INCLUDE_DIR "${WHISPER_SDK_ROOT}/include/npu/rpc/linux")
else()
    message(FATAL_ERROR
        "Could not find Whisper headers under ${WHISPER_SDK_ROOT}. "
        "Expected include/cpp/Whisper.h (SDK 2.6+) or include/npu/rpc/linux/Whisper.h (SDK 2.5).")
endif()

set(WHISPER_LIB_DIR
    "${WHISPER_SDK_ROOT}/libs/npu/rpc_libraries/linux/whisper_all_quantized")

# ------------------------------------------------------------------------------
# cpp-httplib (header-only HTTP framework – replaces Crow + Asio)
#
# Expected to be present as a directory containing httplib.h:
#   ln -s /path/to/cpp-httplib  <src_dir>/cpp-httplib
# or
#   cmake -DHTTPLIB_ROOT=/path/to/cpp-httplib ..
# ------------------------------------------------------------------------------
set(HTTPLIB_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp-httplib"
    CACHE PATH "Path to the cpp-httplib directory (contains httplib.h)")

# ------------------------------------------------------------------------------
# nlohmann/json (system package: nlohmann-json3-dev)
# ------------------------------------------------------------------------------
find_package(nlohmann_json REQUIRED)

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
include_directories(
    ${WHISPER_INCLUDE_DIR}
    ${HTTPLIB_ROOT}
)

# ------------------------------------------------------------------------------
# Whisper SDK imported shared libraries
# ------------------------------------------------------------------------------
foreach(_lib whisperfunction whisper_lib dnnvad  fft)
    add_library(${_lib} SHARED IMPORTED)
    set_target_properties(${_lib} PROPERTIES
        IMPORTED_LOCATION "${WHISPER_LIB_DIR}/lib${_lib}.so")
endforeach()

# ------------------------------------------------------------------------------
# Link
# ------------------------------------------------------------------------------
target_link_libraries(${APP}
    PRIVATE
        whisperfunction
        whisper_lib
        dnnvad
        fft
        nlohmann_json::nlohmann_json
)

# ------------------------------------------------------------------------------
# Configuration summary
# ------------------------------------------------------------------------------
message(STATUS "Building        : ${APP}")
message(STATUS "Whisper SDK     : ${WHISPER_SDK_ROOT}")
message(STATUS "Whisper include : ${WHISPER_INCLUDE_DIR}")
message(STATUS "Whisper libs    : ${WHISPER_LIB_DIR}")
message(STATUS "cpp-httplib     : ${HTTPLIB_ROOT}")
