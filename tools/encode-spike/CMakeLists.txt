# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The Phase-0 go/no-go gate: capture -> colour convert -> encode -> mux, with synced audio.
# See docs/adr/0006-nvenc-interop-path.md for the encoder ladder this walks.

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec libavformat libavutil libswscale libswresample)

add_executable(encode-spike
    main.cpp
    encoder.cpp
    audio.cpp
)

target_link_libraries(encode-spike PRIVATE
    libertas::spike-core
    PkgConfig::FFMPEG
)
