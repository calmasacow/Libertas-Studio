// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A label that appears beside its anchor. Deliberately not a popup window: ADR-0015 says the
/// workspace is one surface, and a tooltip that is its own toplevel is one more thing a
/// compositor can place somewhere surprising.
Rectangle {
    id: tip

    required property Item anchorItem
    property alias text: label.text

    parent: tip.anchorItem ? tip.anchorItem.Window.contentItem : null
    x: tip.anchorItem ? Math.min(tip.anchorItem.mapToItem(parent, 0, 0).x,
                                 parent.width - width - 4) : 0
    y: tip.anchorItem ? tip.anchorItem.mapToItem(parent, 0, 0).y - height - 4 : 0
    z: 500
    width: label.implicitWidth + 12
    height: label.implicitHeight + 8
    radius: 3
    color: Theme.c.panelHeaderActive
    border.width: 1
    border.color: Theme.c.border
    opacity: visible ? 1 : 0

    Behavior on opacity {
        NumberAnimation {
            duration: 90
        }
    }

    Text {
        id: label

        anchors.centerIn: parent
        font.pixelSize: 11
        color: Theme.c.text
    }
}
