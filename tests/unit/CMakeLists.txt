# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

# ltest.hpp is the whole harness; see the comment at the top of it for why there is no framework.
add_library(ltest INTERFACE)
add_library(libertas::ltest ALIAS ltest)
target_include_directories(ltest INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ltest INTERFACE libertas::flags)

function(libertas_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE libertas::ltest ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

libertas_unit_test(test_dropzone        libertas::llayout)
libertas_unit_test(test_layout          libertas::llayout)
libertas_unit_test(test_layout_history  libertas::llayout)
libertas_unit_test(test_layout_serialize libertas::llayout libertas::lcore)
libertas_unit_test(test_theme            libertas::ltheme libertas::lcore)
