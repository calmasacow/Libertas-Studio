# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Phase-0 capture spike: portal ScreenCast -> PipeWire -> Vulkan dmabuf import.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
find_package(Vulkan REQUIRED)

add_executable(capture-spike
    main.cpp
    portal.cpp
    stream.cpp
)

target_link_libraries(capture-spike PRIVATE
    libertas::flags
    libertas::lcore
    PkgConfig::GIO_UNIX
    PkgConfig::PIPEWIRE
    PkgConfig::LIBDRM
    Vulkan::Vulkan
)
