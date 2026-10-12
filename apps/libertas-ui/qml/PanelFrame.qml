// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

// One tabbed panel group, at the rect the layout engine computed for it. This item never decides
// where it goes; it only draws where it was told.
Rectangle {
    id: frame

    required property var panel
    /// The coordinate space drag positions are reported in -- the whole workspace, so a drag
    /// that leaves this panel is still measured against the same origin the engine hit-tests in.
    required property Item surface

    readonly property bool hasPanel: panel && panel.w !== undefined

    x: hasPanel ? panel.x : 0
    y: hasPanel ? panel.y : 0
    width: hasPanel ? panel.w : 0
    height: hasPanel ? panel.h : 0
    visible: hasPanel
    color: Theme.c.panelBackground
    border.width: 1
    border.color: Theme.c.border
    radius: 2
    clip: true

    TabStrip {
        id: strip

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 1
        panel: frame.panel
        surface: frame.surface
    }

    PanelBody {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: strip.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        panelId: frame.hasPanel && frame.panel.activeId !== undefined ? frame.panel.activeId : ""
    }
}
