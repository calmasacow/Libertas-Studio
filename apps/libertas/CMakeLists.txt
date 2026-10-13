# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM   REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(GIO      REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(AVCODEC  REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_package(Vulkan REQUIRED)

add_executable(libertas
    inspect.cpp
    main.cpp
    probe.cpp
)

target_compile_definitions(libertas PRIVATE LIBERTAS_VERSION="${PROJECT_VERSION}")

target_link_libraries(libertas PRIVATE
    libertas::flags
    libertas::lcore
    libertas::ldoc
    Vulkan::Vulkan
    PkgConfig::LIBDRM
    PkgConfig::PIPEWIRE
    PkgConfig::GIO
    PkgConfig::AVCODEC
)

install(TARGETS libertas RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(LIBERTAS_BUILD_TESTS)
    # The inspector reads the committed golden project, which is the only test input in the tree
    # guaranteed to stay valid: if the format changes without the fixture being regenerated, this
    # fails alongside the golden tests rather than quietly reporting nonsense.
    add_test(NAME inspect_golden_project
             COMMAND libertas project
                     "${CMAKE_SOURCE_DIR}/tests/unit/fixtures/golden-project-v1" --verbose)
    set_tests_properties(inspect_golden_project PROPERTIES
        TIMEOUT 30
        PASS_REGULAR_EXPRESSION "emergency scene: Emergency"
        FAIL_REGULAR_EXPRESSION "error "
    )

    add_test(NAME inspect_refuses_a_non_project
             COMMAND libertas project "${CMAKE_CURRENT_BINARY_DIR}/not-a-project")
    set_tests_properties(inspect_refuses_a_non_project PROPERTIES
        TIMEOUT 30
        WILL_FAIL TRUE
    )
endif()
