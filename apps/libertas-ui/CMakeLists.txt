# SPDX-FileCopyrightText: Libertas-Studio contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The workspace shell: one top-level window holding the whole workspace (ADR-0015), with the
# Adobe drop-zone docking gesture (ADR-0016) and live Omarchy theme following.
#
# This target holds no layout logic. Every gesture is a call into libertas::llayout and every
# answer -- including whether a drop is legal -- comes back from there, so what the operator sees
# is what the headless tests cover.

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickShapes)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(libertas-ui
    document.cpp
    document.hpp
    documentmodels.cpp
    documentmodels.hpp
    shortcuts.cpp
    shortcuts.hpp
    layoutmodels.cpp
    layoutmodels.hpp
    main.cpp
    theme.cpp
    theme.hpp
    workspace.cpp
    workspace.hpp
)

# The QML files live in qml/ rather than beside the C++; QTP0004 generates the extra qmldir
# that makes a subdirectory a proper part of the module.
# NoticeLog is a QML-side singleton: a rolling record of everything the shell has said, which
# needs no C++ and no state the session owns.
set_source_files_properties(qml/NoticeLog.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# Must precede qt_add_qml_module: the property is read when the qmldir is generated.
qt_policy(SET QTP0004 NEW)

qt_add_qml_module(libertas-ui
    URI Libertas
    VERSION 1.0
    QML_FILES
        qml/AudioMixerPanel.qml
        qml/CheckLabel.qml
        qml/ChoiceField.qml
        qml/DragGhost.qml
        qml/DropOverlay.qml
        qml/InputsPanel.qml
        qml/LayersPanel.qml
        qml/LogPanel.qml
        qml/Main.qml
        qml/NoticeLog.qml
        qml/NumberField.qml
        qml/PanelBody.qml
        qml/PanelFrame.qml
        qml/PropertiesPanel.qml
        qml/ScenesPanel.qml
        qml/ShortcutsPanel.qml
        qml/StatRow.qml
        qml/StatsPanel.qml
        qml/StatusBar.qml
        qml/TabStrip.qml
        qml/ToolButton.qml
        qml/ToolTipText.qml
)

target_link_libraries(libertas-ui PRIVATE
    libertas::flags
    libertas::llayout
    libertas::lsession
    libertas::ltheme
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickShapes
)

# Qt's generated sources (moc, qrc, the QML type registration) are not ours to keep warning-free,
# and -Werror on a CI build would fail on them rather than on anything we wrote.
set_source_files_properties(
    "${CMAKE_CURRENT_BINARY_DIR}/libertas-ui_qmltyperegistrations.cpp"
    PROPERTIES COMPILE_OPTIONS "-w"
)

# The headless self-check: load the real QML tree, drive the real gestures, touch no screen.
if(LIBERTAS_BUILD_TESTS)
    add_test(NAME shell_self_check COMMAND libertas-ui --check
             --layout "${CMAKE_CURRENT_BINARY_DIR}/self-check-layout.toml"
             --project "${CMAKE_CURRENT_BINARY_DIR}/self-check-project")
    # Qt initialises GL even on the offscreen platform, and the GPU driver leaks a little
    # one-time state on every run. The suppression file names libraries, never functions of ours,
    # so this cannot hide a leak we introduce.
    set_tests_properties(shell_self_check PROPERTIES
        # QT_IM_MODULE=none keeps the input-method plugin out: a headless check has nothing to
        # type into, and loading fcitx leaks 1.7 KB through D-Bus on every run. Not loading it is
        # better than suppressing it.
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_IM_MODULE=none;QT_LOGGING_RULES=qt.qpa.*=false;LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/lsan.suppressions:print_suppressions=0"
        TIMEOUT 60
    )
endif()
