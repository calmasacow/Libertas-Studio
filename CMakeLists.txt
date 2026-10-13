# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28...4.4)

project(LibertasStudio
    VERSION 0.0.1
    DESCRIPTION "Live streaming and recording studio"
    HOMEPAGE_URL "https://github.com/calmasacow/Libertas-Studio"
    LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Keep build trees out of the source tree.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a build/ directory.")
endif()

option(LIBERTAS_WERROR     "Treat warnings as errors"            OFF)
option(LIBERTAS_SANITIZE   "Enable ASan+UBSan"                   OFF)
option(LIBERTAS_TSAN       "Enable ThreadSanitizer"              OFF)
option(LIBERTAS_BUILD_TOOLS "Build spikes and dev tools"         ON)
option(LIBERTAS_BUILD_TESTS "Build tests"                        ON)
set(LIBERTAS_DEPS "auto" CACHE STRING "Dependency strategy: system|vcpkg|auto")

include(LibertasFlags)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "Only Linux is supported at this stage. macOS and Windows arrive in Phase 3 (see the plan).")
endif()

if(LIBERTAS_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(libs/lcore)
add_subdirectory(libs/llayout)
add_subdirectory(libs/ldoc)
add_subdirectory(libs/ltheme)
add_subdirectory(apps/libertas)
add_subdirectory(apps/libertas-ui)

if(LIBERTAS_BUILD_TOOLS)
    add_subdirectory(tools/spike-core)
    add_subdirectory(tools/capture-spike)
    add_subdirectory(tools/encode-spike)
endif()

if(LIBERTAS_BUILD_TESTS)
    add_subdirectory(tests/unit)
endif()

message(STATUS "")
message(STATUS "Libertas-Studio ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  sanitizers : asan/ubsan=${LIBERTAS_SANITIZE} tsan=${LIBERTAS_TSAN}")
message(STATUS "")
