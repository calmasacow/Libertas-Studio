// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The outputs: one encode of one canvas going to one place.
///
/// Its own panel rather than a section of Controls. An operator building a show and an operator
/// running one want different things on screen -- destinations and bitrates are set up once and
/// then wanted out of the way, while program and preview are touched constantly. Keeping them
/// together meant one of the two was always in the way of the other.
///
/// Nothing here encodes anything; there is no render process yet and the footer says so. An
/// output is born disabled, because one that arrived enabled and pointed somewhere would be a
/// keystroke from going to a destination nobody chose.
Item {
    id: panel

    Flickable {
        id: scroll

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        contentHeight: content.height
        contentWidth: width
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content

            width: scroll.width
            spacing: 0

            Repeater {
                model: Document.outputs

                delegate: Item {
                    id: output

                    required property string outputId
                    required property string name
                    required property string canvasName
                    required property string codec
                    required property string implementation
                    required property int bitrateKbps
                    required property string destinationKind
                    required property string url
                    required property string credentialRef
                    required property bool isEnabled
                    required property bool misconfigured

                    objectName: "outputBlock"
                    width: content.width
                    height: outColumn.height + 10
                    property bool expanded: false

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: 2
                        color: output.isEnabled ? Theme.c.panelHeaderActive : "transparent"
                        border.width: 1
                        border.color: output.misconfigured ? Theme.c.error
                                    : output.isEnabled ? Theme.c.good
                                    : Theme.c.border
                    }

                    Column {
                        id: outColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 8
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 3

                        Item {
                            width: parent.width
                            height: 18

                            CheckLabel {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: output.name
                                checked: output.isEnabled
                                onToggled: Document.setOutputEnabled(output.outputId,
                                                                     !output.isEnabled)
                            }

                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4

                                ToolButton {
                                    label: output.expanded ? "▴" : "▾"
                                    tip: qsTr("Encoder and destination")
                                    onClicked: output.expanded = !output.expanded
                                }

                                ToolButton {
                                    label: "−"
                                    tip: qsTr("Remove this output")
                                    onClicked: Document.removeOutput(output.outputId)
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            text: output.canvasName + " · " + output.codec + "/" +
                                  output.implementation + " · " + output.bitrateKbps + " kbps"
                            font.pixelSize: 9
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }

                        Text {
                            width: parent.width
                            visible: output.misconfigured
                            text: qsTr("⚠ enabled with no destination")
                            font.pixelSize: 9
                            color: Theme.c.error
                        }

                        ChoiceField {
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Encoder")
                            choices: {
                                const out = [];
                                for (const c of Document.codecs)
                                    out.push(c.title);
                                return out;
                            }
                            value: {
                                for (const c of Document.codecs) {
                                    if (c.codec === output.codec &&
                                        c.implementation === output.implementation)
                                        return c.title;
                                }
                                return output.codec + "/" + output.implementation;
                            }
                            onCommitted: (newValue) => {
                                for (const c of Document.codecs) {
                                    if (c.title === newValue) {
                                        Document.setOutputEncoder(output.outputId, c.codec,
                                                                  c.implementation);
                                        return;
                                    }
                                }
                            }
                        }

                        NumberField {
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Bitrate")
                            decimals: 0
                            value: output.bitrateKbps
                            onCommitted: (newValue) => Document.setOutputBitrate(
                                output.outputId, Math.max(1, Math.round(newValue)))
                        }

                        ChoiceField {
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Protocol")
                            choices: {
                                const out = [];
                                for (const k of Document.destinationKinds)
                                    out.push(k.title);
                                return out;
                            }
                            value: output.destinationKind.toUpperCase()
                            onCommitted: (newValue) => Document.setOutputDestination(
                                output.outputId, newValue.toLowerCase(), output.url)
                        }

                        Item {
                            width: parent.width
                            visible: output.expanded
                            height: 20

                            Text {
                                id: urlCaption

                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 54
                                text: qsTr("URL")
                                font.pixelSize: 11
                                color: Theme.c.textMuted
                            }

                            Rectangle {
                                anchors.left: urlCaption.right
                                anchors.leftMargin: 4
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 18
                                radius: 2
                                color: Theme.c.window
                                border.width: 1
                                border.color: urlInput.activeFocus
                                    ? Theme.c.accent : Theme.c.border

                                TextInput {
                                    id: urlInput

                                    objectName: "outputUrl"
                                    anchors.fill: parent
                                    anchors.leftMargin: 5
                                    anchors.rightMargin: 5
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 11
                                    color: Theme.c.text
                                    selectionColor: Theme.c.accent
                                    selectByMouse: true
                                    clip: true
                                    text: output.url

                                    onEditingFinished: {
                                        if (text !== output.url)
                                            Document.setOutputDestination(
                                                output.outputId, output.destinationKind, text);
                                    }
                                }
                            }
                        }

                        // The *name* of a credential, never the credential. The project format
                        // has no field that can hold a stream key (ADR-0022), which is why this
                        // says "reference" and why there is no password box anywhere near it.
                        Item {
                            width: parent.width
                            visible: output.expanded
                            height: 20

                            Text {
                                id: credCaption

                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 54
                                text: qsTr("Key ref")
                                font.pixelSize: 11
                                color: Theme.c.textMuted
                            }

                            Rectangle {
                                anchors.left: credCaption.right
                                anchors.leftMargin: 4
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                height: 18
                                radius: 2
                                color: Theme.c.window
                                border.width: 1
                                border.color: credInput.activeFocus
                                    ? Theme.c.accent : Theme.c.border

                                TextInput {
                                    id: credInput

                                    objectName: "outputCredentialRef"
                                    anchors.fill: parent
                                    anchors.leftMargin: 5
                                    anchors.rightMargin: 5
                                    verticalAlignment: Text.AlignVCenter
                                    font.pixelSize: 11
                                    color: Theme.c.text
                                    selectionColor: Theme.c.accent
                                    selectByMouse: true
                                    clip: true
                                    text: output.credentialRef

                                    onEditingFinished: {
                                        if (text !== output.credentialRef)
                                            Document.setOutputCredentialRef(output.outputId,
                                                                            text);
                                    }
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            visible: output.expanded
                            text: qsTr("The key itself is never stored in the project.")
                            font.pixelSize: 9
                            color: Theme.c.textMuted
                            opacity: 0.7
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            Text {
                width: content.width
                visible: Document.outputs.rowCount() === 0
                padding: 10
                text: qsTr("No outputs. An output is one encode of one canvas going to one place.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }
        }
    }

    Rectangle {
        id: footer

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 26
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "+"
                tip: qsTr("Add an output")
                onClicked: Document.addOutput(qsTr("Output"))
            }
        }

        Text {
            anchors.centerIn: parent
            text: qsTr("no render process — nothing is encoding")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
        }
    }
}
