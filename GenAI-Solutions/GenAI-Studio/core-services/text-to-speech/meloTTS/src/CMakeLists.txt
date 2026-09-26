# Copyright (c) 2024-2026 Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.16)
project(tts-service CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
# melo_sdk root – can be overridden on the cmake command line:
#   cmake .. -DMELO_SDK_ROOT=/path/to/melo_sdk
set(MELO_SDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../melo_sdk"
    CACHE PATH "Root directory of the melo_sdk")

set(MELO_SDK_INCLUDE "${MELO_SDK_ROOT}/include/npu/rpc/linux")
set(MELO_SDK_LIB_DIR "${MELO_SDK_ROOT}/libs/npu/rpc_libraries/linux/aarch64")

# Verify the SDK headers exist at configure time
if(NOT EXISTS "${MELO_SDK_INCLUDE}/TTSEngine.h")
    message(FATAL_ERROR
        "melo_sdk headers not found at ${MELO_SDK_INCLUDE}\n"
        "Set -DMELO_SDK_ROOT=<path> to the melo_sdk directory.")
endif()

# ---------------------------------------------------------------------------
# Dependencies (offline-friendly)
# ---------------------------------------------------------------------------
# cpp-httplib is vendored as src/third_party/httplib.h
set(HTTPLIB_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib.h")
if(NOT EXISTS "${HTTPLIB_HEADER}")
    message(FATAL_ERROR
        "Missing ${HTTPLIB_HEADER}. "
        "Please vendor cpp-httplib header before configuring.")
endif()

# nlohmann/json is expected from system package nlohmann-json3-dev
if(NOT EXISTS "/usr/include/nlohmann/json.hpp")
    message(FATAL_ERROR
        "Missing /usr/include/nlohmann/json.hpp. "
        "Install package: nlohmann-json3-dev")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(tts-service
    main.cpp
    TtsApp.cpp
)

target_include_directories(tts-service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # TtsApp.hpp
    ${MELO_SDK_INCLUDE}                  # TTSEngine.h, TTSConfig.h, ...
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party  # httplib.h
    /usr/include                          # nlohmann/json.hpp
)

target_link_directories(tts-service PRIVATE
    ${MELO_SDK_LIB_DIR}
)

target_link_libraries(tts-service PRIVATE
    tts        # libtts.so  (melo_sdk host-side library)
    pthread
)

# Embed the runtime library search path so the binary finds libtts.so
# without needing LD_LIBRARY_PATH to be set manually.
set_target_properties(tts-service PROPERTIES
    INSTALL_RPATH "${MELO_SDK_LIB_DIR}"
    BUILD_RPATH   "${MELO_SDK_LIB_DIR}"
)

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------
target_compile_options(tts-service PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
    -O2
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS tts-service DESTINATION bin)
