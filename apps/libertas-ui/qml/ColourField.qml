// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// A colour, drawn as itself with its hex written on it.
///
/// There is no colour wheel behind a "Select colour" button, because there is no colour wheel
/// in this build and a button that opens nothing is worse than no button. The bar *is* the
/// field: it shows the colour at the size the eye can judge it, and the six digits on it are
/// the thing you type into.
///
/// Values are sRGB hex, which is what a human reads. Everything the document composites in is
/// scene-linear, and the one conversion between them lives in `ldoc::Rgba` (ADR: types.hpp).
Item {
    id: field

    required property string label
    property int labelWidth: 54
    property int labelAlign: Text.AlignLeft
    /// "#rrggbb".
    property string value: "#000000"

    signal committed(string newValue)

    implicitHeight: 22

    readonly property bool parses: /^#?[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$/.test(input.text.trim())
    readonly property color shown: field.parses ? field.swatchColour(input.text) : "transparent"

    function swatchColour(text) {
        const t = text.trim();
        return t.startsWith("#") ? t : "#" + t;
    }

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
        anchors.left: caption.right
        anchors.leftMargin: 4
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 20
        radius: 2
        color: field.parses ? field.shown : Theme.c.window
        border.width: 1
        border.color: input.activeFocus ? Theme.c.accent
                    : !field.parses ? Theme.c.warning
                    : Theme.c.border

        // Checked against the swatch rather than the theme: white text on a pale yellow card
        // is unreadable, and the swatch is whatever the operator typed.
        readonly property color ink: !field.parses ? Theme.c.warning
            : (0.299 * field.shown.r + 0.587 * field.shown.g + 0.114 * field.shown.b) > 0.55
              ? "#101010" : "#f0f0f0"

        TextInput {
            id: input

            anchors.centerIn: parent
            width: parent.width - 12
            horizontalAlignment: TextInput.AlignHCenter
            font.pixelSize: 11
            font.family: "monospace"
            color: parent.ink
            selectionColor: Theme.c.accent
            selectByMouse: true

            text: activeFocus ? text : field.value

            onEditingFinished: {
                if (!field.parses)
                    text = field.value;
                else if (field.swatchColour(text) !== field.value)
                    field.committed(field.swatchColour(text).toLowerCase());
            }
            Keys.onEscapePressed: {
                text = field.value;
                focus = false;
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.IBeamCursor
            acceptedButtons: Qt.LeftButton
            onClicked: {
                input.forceActiveFocus();
                input.selectAll();
            }
            // The text input sits on top and takes its own clicks; this is only the margin
            // around it, so clicking anywhere on the bar starts an edit.
            z: -1
        }
    }
}
