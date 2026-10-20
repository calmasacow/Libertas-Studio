# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
add_library(limport STATIC src/obs.cpp src/obs_export.cpp)
add_library(libertas::limport ALIAS limport)
target_include_directories(limport PUBLIC include)
target_link_libraries(limport PUBLIC libertas::ldoc PRIVATE libertas::lcomp libertas::lcore Qt6::Gui PkgConfig::JSONCPP)
