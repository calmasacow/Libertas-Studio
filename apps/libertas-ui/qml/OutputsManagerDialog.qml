// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Output settings and the encodes shared by their destinations.
Item {
    id: dialog

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    property string selectedOutputId: ""

    function openOutput(outputId) {
        open();
        selectedOutputId = outputId;
        Qt.callLater(function() {
            content.forceLayout();
            for (let i = 0; i < outputEntries.count; ++i) {
                const entry = outputEntries.itemAt(i);
                if (entry.outputId === dialog.selectedOutputId) {
                    scroll.contentY = Math.max(0, Math.min(entry.y,
                        scroll.contentHeight - scroll.height));
                    entry.forceActiveFocus();
                    break;
                }
            }
        });
    }

    function open() {
        selectedOutputId = "";
        scroll.contentY = 0;
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
                    id: outputEntries
                    model: Document.outputs

                    delegate: Column {
                        id: output
                        required property string outputId
                        required property string name
                        required property string canvasName
                        required property string encodeName
                        required property string resolution
                        required property var sharedWith
                        required property string codec
                        required property string implementation
                        required property int bitrateKbps
                        required property string destinationKind
                        required property string url
                        required property string credentialRef
                        required property bool isEnabled
                        required property bool misconfigured

                        objectName: "managedOutput"
                        width: content.width - 20
                        x: 10
                        spacing: 6
                        property bool expanded: dialog.selectedOutputId === outputId

                        ToolButton {
                            width: parent.width
                            height: 32
                            label: output.name
                            tip: qsTr("Settings for %1").arg(output.name)
                            active: output.expanded
                            onClicked: dialog.selectedOutputId = output.expanded ? "" : output.outputId
                        }
                        CheckLabel {
                            visible: output.expanded
                            text: qsTr("Include in output configuration (not running)")
                            checked: output.isEnabled
                            onToggled: Document.setOutputEnabled(output.outputId, !output.isEnabled)
                        }
                        Text {
                            visible: output.expanded
                            width: parent.width
                            text: output.resolution + " / " + output.bitrateKbps + " kbps" +
                                  (output.sharedWith.length ? qsTr(" - shared with %1").arg(output.sharedWith.join(", ")) : "")
                            font.pixelSize: 10
                            color: Theme.c.textMuted
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            visible: output.expanded && output.misconfigured
                            text: qsTr("Enabled with no destination")
                            color: Theme.c.error
                            font.pixelSize: 10
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
