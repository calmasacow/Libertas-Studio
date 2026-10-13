// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

Row {
    id: check

    property alias text: label.text
    property bool checked: false

    signal toggled

    spacing: 6

    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        width: 13
        height: 13
        radius: 2
        color: check.checked ? Theme.c.accent : "transparent"
        border.width: 1
        border.color: mouse.containsMouse ? Theme.c.accent : Theme.c.border

        Text {
            anchors.centerIn: parent
            visible: check.checked
            text: "✓"
            font.pixelSize: 10
            color: Theme.c.window
        }
    }

    Text {
        id: label

        anchors.verticalCenter: parent.verticalCenter
        font.pixelSize: 11
        color: Theme.c.text
    }

    MouseArea {
        id: mouse

        anchors.fill: parent
        hoverEnabled: true
        onClicked: check.toggled()
    }
}
