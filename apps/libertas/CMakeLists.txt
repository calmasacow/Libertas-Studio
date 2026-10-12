# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(GIO      REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_package(Vulkan REQUIRED)

add_executable(libertas
    main.cpp
    probe.cpp
)

target_compile_definitions(libertas PRIVATE LIBERTAS_VERSION="${PROJECT_VERSION}")

target_link_libraries(libertas PRIVATE
    libertas::flags
    libertas::lcore
    Vulkan::Vulkan
    PkgConfig::LIBDRM
    PkgConfig::PIPEWIRE
    PkgConfig::GIO
    PkgConfig::AVCODEC
)

install(TARGETS libertas RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
