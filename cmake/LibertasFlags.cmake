# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Shared compile/link settings, exposed as the INTERFACE target libertas::flags.

add_library(libertas_flags INTERFACE)
add_library(libertas::flags ALIAS libertas_flags)

# -Wnon-virtual-dtor and -Woverloaded-virtual are C++-only, and generated Wayland protocol code
# is C: without this guard every one of those .c files compiles with a cc1 "valid for C++ but not
# for C" warning, which is noise in every build log on every machine.
target_compile_options(libertas_flags INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
)

target_compile_options(libertas_flags INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow
    -Wcast-align
    -Wunused
    -Wconversion -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    # Crash reports must be symbolisable: see ADR-0005.
    -fno-omit-frame-pointer
    -g
)

if(LIBERTAS_WERROR)
    target_compile_options(libertas_flags INTERFACE -Werror)
endif()

# No exceptions cross an IPC boundary (ADR-0005). We still use them in-process, but every
# worker_main() and message handler wraps its body in a catch-all.

if(LIBERTAS_SANITIZE AND LIBERTAS_TSAN)
    message(FATAL_ERROR "LIBERTAS_SANITIZE and LIBERTAS_TSAN are mutually exclusive")
endif()

if(LIBERTAS_SANITIZE)
    target_compile_options(libertas_flags INTERFACE
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    target_link_options(libertas_flags INTERFACE -fsanitize=address,undefined)
endif()

if(LIBERTAS_TSAN)
    target_compile_options(libertas_flags INTERFACE -fsanitize=thread)
    target_link_options(libertas_flags INTERFACE -fsanitize=thread)
endif()

# Split debug info keeps binaries small while staying debuggable with systemd-coredump.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-gsplit-dwarf" LIBERTAS_HAS_SPLIT_DWARF)
if(LIBERTAS_HAS_SPLIT_DWARF AND CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(libertas_flags INTERFACE -gsplit-dwarf)
endif()

target_link_options(libertas_flags INTERFACE
    LINKER:--build-id
    LINKER:--as-needed
)
