# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The encoder: capture or composite in, a playable file out. ADR-0006 is the encoder ladder
# this walks; ADR-0005 puts it in the render process, which is where the GPU already is.

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample)

add_library(lencode
    src/encoder.cpp
    src/config.cpp
)
add_library(libertas::lencode ALIAS lencode)

target_include_directories(lencode PUBLIC include)
target_link_libraries(lencode
    PUBLIC
        libertas::ldoc
    PRIVATE
        libertas::flags
        libertas::lcore
        PkgConfig::FFMPEG
)
