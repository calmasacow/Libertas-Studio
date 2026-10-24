// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// One node on the encode graph: a titled card with a body and ports on its edges.
///
/// One component for all five kinds -- canvas, audio track, encoder, audio encoder,
/// destination -- because they differ only in what goes in the body. Five near-identical
/// cards would drift apart the first time the selection outline changed.
///
/// The card does not know what it represents. It reports presses and drags; whoever placed it
/// decides what those mean.
Item {
    id: card

    required property string title
    /// Drawn after the title in the muted colour: a resolution, a bitrate, a codec.
    property string subtitle: ""
    property bool selected: false
    /// Whether to draw a port on the left edge, the right edge, or both. A canvas has no
    /// input and a destination has no output, and drawing one anyway would promise an edge
    /// that cannot be made.
    property bool hasInput: false
    property bool hasOutput: false
    /// Where edges attach, in the graph's coordinates. Read by GraphEdges.
    readonly property point inPort: Qt.point(card.x, card.y + 30)
    readonly property point outPort: Qt.point(card.x + card.width, card.y + 30)
    /// Filled by the node, drawn at the right of the title row.
    property alias status: statusSlot.data
    default property alias body: bodySlot.data

    signal pressed(real mouseX, real mouseY)
    signal dragged(real mouseX, real mouseY)
    signal released
    signal contextRequested(real mouseX, real mouseY)

    implicitWidth: 260
    implicitHeight: header.height + bodySlot.implicitHeight + 10

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: Theme.c.panelBackground
        border.width: 1
        border.color: card.selected ? Theme.c.accent : Theme.c.border
    }

    // The ports. Small, and drawn on the border rather than beside it, so an edge meeting one
    // lands on the card's own outline instead of floating off it.
    Repeater {
        model: [{ "show": card.hasInput, "at": 0 }, { "show": card.hasOutput, "at": card.width }]

        delegate: Rectangle {
            required property var modelData

            visible: modelData.show
            x: modelData.at - 3
            y: 27
            width: 6
            height: 6
            radius: 3
            color: card.selected ? Theme.c.accent : Theme.c.textMuted
        }
    }

    Item {
        id: header

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 24

        Text {
            id: titleText

            anchors.left: parent.left
            anchors.leftMargin: 9
            anchors.verticalCenter: parent.verticalCenter
            text: card.title
            font.pixelSize: 11
            font.weight: Font.DemiBold
            color: Theme.c.text
            elide: Text.ElideRight
            width: Math.min(implicitWidth, parent.width - statusSlot.width - 18)
        }

        Text {
            anchors.left: titleText.right
            anchors.leftMargin: 6
            anchors.right: statusSlot.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: card.subtitle !== ""
            text: card.subtitle
            font.pixelSize: 10
            color: Theme.c.textMuted
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
        }

        Item {
            id: statusSlot

            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
            width: implicitWidth
            height: implicitHeight
        }
    }

    Item {
        id: bodySlot

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 1
        implicitHeight: childrenRect.height + 8
    }

    // Above the card, below its body: a press anywhere that is not a control selects and
    // starts a drag, and a control inside the body takes its own events first.
    MouseArea {
        anchors.fill: parent
        z: -1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton)
                card.contextRequested(mouse.x, mouse.y);
            else
                card.pressed(mouse.x, mouse.y);
        }
        onPositionChanged: mouse => {
            if (pressedButtons & Qt.LeftButton)
                card.dragged(mouse.x, mouse.y);
        }
        onReleased: mouse => {
            if (mouse.button === Qt.LeftButton)
                card.released();
        }
        onCanceled: card.released
    }
}
