# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The document model: the non-OBS-shaped scene/layer/bus/canvas model, shared by the UI, the
# session process and the OBS importer. No Qt, no GPU, no FFmpeg -- the model has to be usable
# from a headless importer and testable without any of them.

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)

add_library(ldoc STATIC
    src/document.cpp
    src/json.cpp
    src/project_io.cpp
    src/node.cpp
    src/types.cpp
)
add_library(libertas::ldoc ALIAS ldoc)

target_include_directories(ldoc PUBLIC include)
target_link_libraries(ldoc
    PUBLIC libertas::flags libertas::lcore
    PRIVATE PkgConfig::JSONCPP
)
