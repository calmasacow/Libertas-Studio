# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Qt6 REQUIRED COMPONENTS Gui Multimedia Widgets WebEngineWidgets)
add_executable(libertas-source main.cpp)
target_link_libraries(libertas-source PRIVATE libertas::flags libertas::lcomp
    Qt6::Gui Qt6::Multimedia Qt6::Widgets Qt6::WebEngineWidgets)
