# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(libertas-vst main.cpp)
target_link_libraries(libertas-vst PRIVATE libertas::flags ${CMAKE_DL_LIBS})
