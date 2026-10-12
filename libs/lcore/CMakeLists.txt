# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(lcore STATIC
    src/atomic_file.cpp
    src/log.cpp
)
add_library(libertas::lcore ALIAS lcore)

target_include_directories(lcore PUBLIC include)
target_link_libraries(lcore PUBLIC libertas::flags)
