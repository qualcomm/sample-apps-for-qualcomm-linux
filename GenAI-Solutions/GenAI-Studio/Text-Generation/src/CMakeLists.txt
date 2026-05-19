#==============================================================================
#
#  Copyright (c) Qualcomm Technologies, Inc.
#  All Rights Reserved.
#  Confidential and Proprietary - Qualcomm Technologies, Inc.
#
#==============================================================================

cmake_minimum_required(VERSION 3.18.2)
project(chat_app)

# Application name
set(APP "chat_app")

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
add_executable(${APP}
    Main.cpp
    ChatApp.cpp
    Genie.cpp
    Logger.cpp
)

# ------------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------------

# JSON library (header-only)
find_package(nlohmann_json REQUIRED)

# Crow and Asio paths
set(CROW_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/crow)
set(ASIO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/asio/asio)

# QNN SDK paths
set(CHIPSET "QCS8275")
set(QNN_ROOT "/app/Text-Generation/src/qairt/${QAIRT_VER}")
set(QNN_INCLUDE_DIR "${QNN_ROOT}/include/Genie")
set(QNN_LIB_PREFIX "${QNN_ROOT}/lib")
set(_dtuple_POSTFIX "oe-linux-gcc11.2")

# Get QNN library path
get_filename_component(QNN_LIB_PATH
    "${QNN_LIB_PREFIX}/aarch64-${_dtuple_POSTFIX}/libGenie.so"
    REALPATH BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}
)

# ------------------------------------------------------------------------------
# Imported QNN target
# ------------------------------------------------------------------------------
add_library(QNN SHARED IMPORTED)
set_target_properties(QNN PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES ${QNN_INCLUDE_DIR}
    IMPORTED_LOCATION ${QNN_LIB_PATH}
)

# ------------------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------------------
include_directories(${QNN_INCLUDE_DIR})
include_directories(${CROW_SDK_ROOT}/include)
include_directories(${ASIO_ROOT}/include)

# ------------------------------------------------------------------------------
# Link libraries
# ------------------------------------------------------------------------------
target_link_libraries(${APP}
    PUBLIC QNN
    PRIVATE nlohmann_json::nlohmann_json
)

# ------------------------------------------------------------------------------
# Optional: Print configuration summary
# ------------------------------------------------------------------------------
message(STATUS "Building ${APP}")
message(STATUS "QNN SDK Root: ${QNN_ROOT}")
message(STATUS "QNN Include: ${QNN_INCLUDE_DIR}")
message(STATUS "QNN Library: ${QNN_LIB_PATH}")
