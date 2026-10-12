# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Phase-0 capture spike: portal ScreenCast -> PipeWire -> Vulkan dmabuf import.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(WAYLAND  REQUIRED IMPORTED_TARGET wayland-client)
find_package(Vulkan REQUIRED)

add_executable(capture-spike
    main.cpp
    portal.cpp
    stream.cpp
    vk_import.cpp
    wlcap.cpp
)

# ext-image-copy-capture-v1 is the primary Linux capture backend (ADR-0011); the portal path above
# stays as the universal fallback.
include(WaylandProtocol)
# ext-image-capture-source references ext_foreign_toplevel_handle_v1 for window sources, so that
# protocol must be generated too - and it is what window capture by identity will be built on.
libertas_wayland_protocol(capture-spike staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml)
libertas_wayland_protocol(capture-spike staging/ext-image-capture-source/ext-image-capture-source-v1.xml)
libertas_wayland_protocol(capture-spike staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml)
libertas_wayland_protocol(capture-spike stable/linux-dmabuf/linux-dmabuf-v1.xml)

target_link_libraries(capture-spike PRIVATE
    libertas::flags
    libertas::lcore
    PkgConfig::GIO_UNIX
    PkgConfig::PIPEWIRE
    PkgConfig::LIBDRM
    PkgConfig::WAYLAND
    Vulkan::Vulkan
)
