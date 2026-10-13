// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A numeric field that commits when editing finishes, not while it is being typed.
///
/// Deliberately not a slider. Every committed edit is one undoable command, and a control that
/// emits a value per pixel of travel would put two hundred of them on the undo stack for one
/// drag. Continuous controls need the session to grow a way to coalesce a run of commands into
/// one step; until it has that, the honest control is one that commits once.
Item {
    id: field

    required property string label
    property real value: 0
    property int decimals: 2

    signal committed(real newValue)

    implicitHeight: 20

    Text {
        id: caption

        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: 54
        text: field.label
        font.pixelSize: 11
        color: Theme.c.textMuted
        elide: Text.ElideRight
    }

    Rectangle {
        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 18
        radius: 2
        color: field.enabled ? Theme.c.window : "transparent"
        border.width: 1
        border.color: input.activeFocus ? Theme.c.accent
                    : hover.containsMouse ? Theme.c.border
                    : "transparent"

        MouseArea {
            id: hover

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: field.enabled ? Qt.IBeamCursor : Qt.ArrowCursor
            acceptedButtons: Qt.NoButton
        }

        TextInput {
            id: input

            anchors.fill: parent
            anchors.leftMargin: 5
            anchors.rightMargin: 5
            verticalAlignment: TextInput.AlignVCenter
            font.pixelSize: 11
            color: field.enabled ? Theme.c.text : Theme.c.textMuted
            selectionColor: Theme.c.accent
            selectByMouse: true
            validator: DoubleValidator {}

            // Only follow the document while the operator is not typing, or their half-finished
            // number gets replaced the moment anything else changes.
            text: activeFocus ? text : field.value.toFixed(field.decimals)

            onEditingFinished: {
                const parsed = parseFloat(text);
                if (!isNaN(parsed) && parsed !== field.value)
                    field.committed(parsed);
                else
                    text = field.value.toFixed(field.decimals);
            }
            Keys.onEscapePressed: {
                text = field.value.toFixed(field.decimals);
                focus = false;
            }
        }
    }
}
