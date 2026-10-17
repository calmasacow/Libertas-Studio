// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The right-click menu on a mixer strip.
///
/// Drawn rather than taken from QtQuick.Controls' Menu, for the reason ADR-0015 gives for the
/// rest of the shell: a styled popup brings a platform style to fight with the theme, for the
/// sake of a rectangle and two rows of text. If a second panel wants a context menu this
/// becomes a shared component rather than being copied (ADR-0032).
///
/// Placed by its caller in the panel's coordinates and kept inside it: a menu opened on the
/// rightmost strip must not hang off the edge of a docked panel, where there is no window to
/// spill into.
Item {
    id: menu

    /// Rows: {text, checked, action}. `action` is called with no arguments.
    property var items: []

    anchors.fill: parent
    visible: false

    function popup(x, y, rows) {
        menu.items = rows;
        frame.x = Math.max(4, Math.min(x, menu.width - frame.width - 4));
        frame.y = Math.max(4, Math.min(y, menu.height - frame.height - 4));
        menu.visible = true;
        menu.forceActiveFocus();
    }

    function close() {
        menu.visible = false;
    }

    Keys.onEscapePressed: menu.close()

    // Catches the click that dismisses it, including a right-click somewhere else.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: menu.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        id: frame

        objectName: "stripMenu"
        width: 150
        height: rows.height + 8
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        Column {
            id: rows

            y: 4
            width: parent.width

            Repeater {
                model: menu.items

                delegate: Rectangle {
                    id: row

                    required property var modelData

                    objectName: "stripMenuItem"
                    width: rows.width
                    height: 24
                    color: rowMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"

                    // A tick rather than a checkbox: these are states the strip is already
                    // showing, and the menu is confirming rather than offering.
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: 12
                        text: row.modelData.checked ? "✓" : ""
                        font.pixelSize: 10
                        color: Theme.c.accent
                    }

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 24
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.text
                        font.pixelSize: 11
                        color: Theme.c.text
                        elide: Text.ElideRight
                    }

                    MouseArea {
                        id: rowMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            row.modelData.action();
                            menu.close();
                        }
                    }
                }
            }
        }
    }
}
