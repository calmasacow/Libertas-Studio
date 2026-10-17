// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The encodes, and the destinations hanging off each one.
///
/// Encode-centred on purpose. The outputs panel is a list of destinations, which is the right
/// shape for "where does this go" and the wrong shape for the question this window answers:
/// how many times is the GPU being asked to encode, and which destinations are sharing the
/// result. That is the whole of ADR-0029 -- encode 1440p once for YouTube, encode 1080p once
/// and send it to Twitch and Kick -- and it is invisible in a list sorted by destination,
/// because two rows carrying the same bitrate look exactly like two separate encodes.
///
/// The count at the top is the same number `libertas inspect` prints. If the two ever disagree,
/// one of them is wrong about what the GPU is being asked to do.
Item {
    id: dialog

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open() {
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.closed();
    }

    Keys.onEscapePressed: dialog.close()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        id: frame

        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 620)
        height: Math.min(parent.height - 48, 560)
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        Rectangle {
            id: titleBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: 34
            color: Theme.c.panelHeader

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    width: parent.width
                    text: qsTr("Outputs Manager")
                    font.pixelSize: 11
                    font.bold: true
                    color: Theme.c.text
                }

                Text {
                    width: parent.width
                    text: Document.encodeCount() === 1
                          ? qsTr("1 concurrent encode asked of the GPU")
                          : qsTr("%1 concurrent encodes asked of the GPU")
                                .arg(Document.encodeCount())
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }
            }
        }

        Flickable {
            id: scroll

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleBar.bottom
            anchors.bottom: bottomBar.top
            anchors.margins: 1
            clip: true
            contentHeight: content.height
            contentWidth: width
            boundsBehavior: Flickable.StopAtBounds
            // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
            // because Flickable drops it along with the drag.
            interactive: false

            NoDragScroll {}

            Column {
                id: content

                width: scroll.width
                spacing: 0

                Repeater {
                    model: Document.encodes

                    delegate: Column {
                        id: encodeBlock

                        required property string encodeId
                        required property string name
                        required property string canvasId
                        required property string canvasName
                        required property string resolution
                        required property string codec
                        required property string implementation
                        required property int bitrateKbps
                        required property string preset
                        required property var destinations

                        objectName: "encodeBlock"
                        width: content.width
                        spacing: 0

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: Theme.c.border
                        }

                        Item {
                            width: parent.width
                            height: 40

                            // Which canvas this encode reads. Without it a second canvas can
                            // never be streamed: addEncode takes whichever canvas was current
                            // and nothing could move it afterwards (ADR-0035).
                            ChoiceField {
                                id: canvasPick

                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                width: 190
                                label: qsTr("Canvas")
                                labelWidth: 48
                                choices: Document.canvasNames()
                                value: encodeBlock.canvasName
                                onCommitted: (v) => {
                                    const ids = Document.canvasIds();
                                    const names = Document.canvasNames();
                                    for (let i = 0; i < names.length; ++i) {
                                        if (names[i] === v)
                                            Document.setEncodeCanvas(encodeBlock.encodeId,
                                                                     ids[i]);
                                    }
                                }
                            }

                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.right: canvasPick.left
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1

                                Text {
                                    width: parent.width
                                    text: encodeBlock.name
                                    font.pixelSize: 11
                                    font.bold: true
                                    color: Theme.c.text
                                    elide: Text.ElideRight
                                }

                                Text {
                                    width: parent.width
                                    text: encodeBlock.canvasName + " → " + encodeBlock.resolution
                                          + " · " + encodeBlock.codec + "/"
                                          + encodeBlock.implementation + " · "
                                          + encodeBlock.bitrateKbps + " kbps"
                                          + (encodeBlock.preset === ""
                                             ? "" : " · " + encodeBlock.preset)
                                    font.pixelSize: 9
                                    color: Theme.c.textMuted
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        // The destinations on this encode. More than one here is the reuse
                        // this window exists to make visible -- one pass of the GPU, several
                        // places it goes.
                        Repeater {
                            model: encodeBlock.destinations

                            delegate: Item {
                                id: destRow

                                required property string modelData

                                width: content.width
                                height: 20

                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 24
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "└ " + destRow.modelData
                                    font.pixelSize: 10
                                    color: Theme.c.text
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            leftPadding: 24
                            bottomPadding: 2
                            visible: encodeBlock.destinations.length === 0
                            text: qsTr("no destination — this encode goes nowhere")
                            font.pixelSize: 9
                            color: Theme.c.warning
                        }

                        Item {
                            width: parent.width
                            height: 28

                            DialogButton {
                                anchors.left: parent.left
                                anchors.leftMargin: 24
                                anchors.verticalCenter: parent.verticalCenter
                                objectName: "addDestination"
                                minimumWidth: 110
                                text: qsTr("+ destination")
                                // The Aitum case, said plainly: a new destination that reuses
                                // this bitstream rather than asking the GPU for another one.
                                onClicked: Document.addOutputOnEncode(encodeBlock.encodeId,
                                                                      qsTr("Output"))
                            }
                        }
                    }
                }

                Text {
                    width: content.width
                    visible: Document.encodes.rowCount() === 0
                    padding: 10
                    text: qsTr("No encodes yet. An encode is one compressed stream of one "
                               + "canvas; destinations are what receive it.")
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                    wrapMode: Text.WordWrap
                }
            }
        }

        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 40
            color: Theme.c.panelHeader

            DialogButton {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                objectName: "addEncode"
                minimumWidth: 100
                text: qsTr("+ new encode")
                onClicked: Document.addEncode()
            }

            Text {
                anchors.centerIn: parent
                text: qsTr("no render process — nothing is encoding")
                font.pixelSize: 9
                color: Theme.c.textMuted
                opacity: 0.6
            }

            DialogButton {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                primary: true
                onClicked: dialog.close()
            }
        }
    }
}
