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

libertas_unit_test(test_actions          libertas::lsession)
libertas_unit_test(test_command          libertas::ldoc)
libertas_unit_test(test_composite        libertas::lcomp)
libertas_unit_test(test_document         libertas::ldoc)
libertas_unit_test(test_dropzone        libertas::llayout)
libertas_unit_test(test_journal          libertas::ldoc libertas::lcore)
libertas_unit_test(test_fuzz             libertas::ldoc libertas::llayout libertas::lsession)
libertas_unit_test(test_golden           libertas::ldoc libertas::lcore)
target_compile_definitions(test_golden PRIVATE
    LIBERTAS_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
libertas_unit_test(test_ipc              libertas::lipc)
libertas_unit_test(test_json             libertas::ldoc)
libertas_unit_test(test_preview         libertas::lcomp)
target_compile_definitions(test_preview PRIVATE
    LIBERTAS_RENDER="$<TARGET_FILE:libertas-render>")
add_dependencies(test_preview libertas-render)
libertas_unit_test(test_project_io       libertas::ldoc libertas::lcore)
libertas_unit_test(test_layout          libertas::llayout)
libertas_unit_test(test_layout_history  libertas::llayout)
libertas_unit_test(test_layout_serialize libertas::llayout libertas::lcore)
libertas_unit_test(test_session          libertas::lsession)
libertas_unit_test(test_theme            libertas::ltheme libertas::lcore)
libertas_unit_test(test_workspaces       libertas::llayout libertas::lcore)
libertas_unit_test(test_ulid             libertas::lcore)
