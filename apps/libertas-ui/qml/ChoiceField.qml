// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A dropdown, drawn here rather than taken from QtQuick.Controls.
///
/// The list is parented to the window's content item rather than to this field, because a panel
/// clips its contents and a dropdown that is clipped by the panel it lives in is useless. It is
/// not a separate window either: ADR-0015 keeps the workspace to one surface, so there is nothing
/// for a compositor to place somewhere surprising.
Item {
    id: field

    required property string label
    property var choices: []
    property var choiceLabels: ({})
    property string value: ""
    /// See NumberField: a dialog can afford a wider caption column than a docked panel can.
    property int labelWidth: 54
    property int labelAlign: Text.AlignLeft

    signal committed(string newValue)

    implicitHeight: 20

    onVisibleChanged: if (!visible) popup.visible = false

    Text {
        id: caption

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: field.labelWidth
        horizontalAlignment: field.labelAlign
        text: field.label
        font.pixelSize: 11
        color: Theme.c.textMuted
        elide: Text.ElideRight
    }

    Rectangle {
        id: box

        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 18
        radius: 2
        color: field.enabled ? Theme.c.window : "transparent"
        border.width: 1
        border.color: popup.visible ? Theme.c.accent
                    : mouse.containsMouse ? Theme.c.border
                    : "transparent"

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 5
            anchors.right: arrow.left
            anchors.verticalCenter: parent.verticalCenter
            text: field.choiceLabels[field.value] || field.value
            font.pixelSize: 11
            color: field.enabled ? Theme.c.text : Theme.c.textMuted
            elide: Text.ElideRight
        }

        Text {
            id: arrow

            anchors.right: parent.right
            anchors.rightMargin: 5
            anchors.verticalCenter: parent.verticalCenter
            text: "▾"
            font.pixelSize: 9
            color: Theme.c.textMuted
        }

        MouseArea {
            id: mouse

            anchors.fill: parent
            hoverEnabled: true
            onClicked: popup.visible = !popup.visible
        }
    }

    // Dismiss-on-click-outside, covering the whole window beneath the list.
    MouseArea {
        parent: field.Window.contentItem
        anchors.fill: parent
        visible: popup.visible
        z: 400
        onClicked: popup.visible = false
    }

    Rectangle {
        id: popup

        objectName: "choicePopup"
        parent: field.Window.contentItem
        visible: false
        z: 401
        width: Math.max(box.width, 140)
        height: Math.min(choiceList.contentHeight + 2, 260)
        color: Theme.c.panelHeaderActive
        border.width: 1
        border.color: Theme.c.accent
        radius: 2

        function place() {
            const at = box.mapToItem(parent, 0, box.height);
            x = Math.max(2, Math.min(at.x, parent.width - width - 2));
            // Flip above when there is no room below, so the last entry is never off-screen.
            y = at.y + height > parent.height - 2
                 ? Math.max(2, at.y - box.height - height)
                 : at.y + 2;
        }

        onVisibleChanged: {
            if (visible) {
                choiceList.forceLayout();
                place();
            }
        }

        ListView {
            id: choiceList

            anchors.fill: parent
            anchors.margins: 1
            clip: true
            model: field.choices
            boundsBehavior: Flickable.StopAtBounds
            // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
            // because Flickable drops it along with the drag.
            interactive: false

            NoDragScroll {}
            currentIndex: field.choices.indexOf(field.value)

            delegate: Rectangle {
                id: choice

                required property int index
                required property string modelData

                width: choiceList.width
                height: 20
                color: choice.modelData === field.value ? Theme.c.accent
                     : choiceMouse.containsMouse ? Theme.c.panelHeader
                     : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 7
                    anchors.verticalCenter: parent.verticalCenter
                    text: field.choiceLabels[choice.modelData] || choice.modelData
                    font.pixelSize: 11
                    color: choice.modelData === field.value ? Theme.c.window : Theme.c.text
                }

                MouseArea {
                    id: choiceMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        popup.visible = false;
                        if (choice.modelData !== field.value)
                            field.committed(choice.modelData);
                    }
                }
            }
        }
    }
}
