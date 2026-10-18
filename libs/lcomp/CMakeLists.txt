# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The scene walk and the preview that carries its picture to another process.
#
# No Qt, no GPU, no capture. A colour, a group and a precomp can be drawn here, and everything
# else is reported as a gap rather than approximated (ADR-0040). The encode target is still a
# Vulkan composite in the render process; this library does not own a device.

add_library(lcomp STATIC
    src/composite.cpp
    src/preview.cpp
)
add_library(libertas::lcomp ALIAS lcomp)

target_include_directories(lcomp PUBLIC include)
target_link_libraries(lcomp
    PUBLIC libertas::flags libertas::ldoc libertas::lipc
)
