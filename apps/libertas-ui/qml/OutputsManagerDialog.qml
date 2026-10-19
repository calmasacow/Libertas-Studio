// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as Controls
import Libertas

/// Output settings and the encodes shared by their destinations.
Item {
    id: dialog

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    property string selectedOutputId: ""
    property string selectedDestinationId: ""
    property int revision: 0
    property var encodeOptions: { revision; return Document.encodeChoices(); }
    Connections {
        target: Document
        function onChanged() { dialog.revision++; }
    }

    component SettingText: Row {
        id: field
        required property string label
        property alias text: input.text
        property alias inputName: input.objectName
        property alias echoMode: input.echoMode
        property alias placeholderText: input.placeholderText
        signal committed(string value)
        spacing: 4
        height: 28
        Text {
            width: 100
            anchors.verticalCenter: parent.verticalCenter
            text: field.label
            color: Theme.c.textMuted
            font.pixelSize: 11
        }
        Controls.TextField {
            id: input
            width: parent.width - 104
            height: 28
            color: Theme.c.text
            font.pixelSize: 11
            selectByMouse: true
            Accessible.name: field.label
            onEditingFinished: field.committed(text)
            background: Rectangle {
                color: Theme.c.window
                border.color: input.activeFocus ? Theme.c.accent : Theme.c.border
                radius: 2
            }
        }
    }

    function openDestination(outputId) {
        openOutput(outputId);
        selectedDestinationId = outputId;
    }

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
        selectedDestinationId = "";
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
                        property var settings: {
                            dialog.revision;
                            return Document.outputEncoderSettings(outputId);
                        }
                        function saveOptions(rate, preset, keyframeMs) {
                            Document.setOutputEncodingOptions(outputId, rate, preset, keyframeMs);
                        }

                        Row {
                            width: parent.width
                            height: 32
                            spacing: 6
                            ToolButton {
                                width: parent.width - 90
                                height: parent.height
                                label: output.name
                                tip: qsTr("Settings for %1").arg(output.name)
                                active: output.expanded
                                onClicked: dialog.selectedOutputId = output.expanded ? "" : output.outputId
                            }
                            ToolButton {
                                objectName: "removeManagedOutput"
                                property string outputId: output.outputId
                                width: 84
                                height: parent.height
                                label: qsTr("Remove")
                                tip: qsTr("Remove %1 (can be undone)").arg(output.name)
                                onClicked: {
                                    if (dialog.selectedOutputId === output.outputId) {
                                        dialog.selectedOutputId = "";
                                        dialog.selectedDestinationId = "";
                                    }
                                    Document.removeOutput(output.outputId);
                                }
                            }
                        }
                        SettingText {
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Name")
                            inputName: "managedOutputName"
                            text: output.name
                            onCommitted: value => Document.renameOutput(output.outputId, value)
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
                            objectName: "outputEncodeChoice"
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Use encode")
                            labelWidth: 100
                            choices: dialog.encodeOptions.map(e => e.id)
                            choiceLabels: {
                                const labels = {};
                                for (const e of dialog.encodeOptions) labels[e.id] = e.name;
                                return labels;
                            }
                            value: output.settings.id || ""
                            onCommitted: value => Document.setOutputEncode(output.outputId, value)
                        }
                        ChoiceField {
                            width: parent.width
                            visible: output.expanded
                            objectName: "outputEncoderChoice"
                            labelWidth: 100
                            label: qsTr("Video encoder")
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
                            objectName: "outputBitrate"
                            labelWidth: 100
                            label: qsTr("Bitrate (kbps)")
                            decimals: 0
                            value: output.bitrateKbps
                            onCommitted: (newValue) => Document.setOutputBitrate(
                                output.outputId, Math.max(1, Math.round(newValue)))
                        }

                        ChoiceField {
                            objectName: "outputRateControl"
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Rate control")
                            labelWidth: 100
                            choices: ["cbr", "vbr", "cqp", "crf"]
                            value: output.settings.rateControl || "cbr"
                            onCommitted: value => output.saveOptions(value, output.settings.preset,
                                                                      output.settings.keyframeMs)
                        }
                        NumberField {
                            objectName: "outputKeyframes"
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Keyframe (s)")
                            labelWidth: 100
                            value: (output.settings.keyframeMs || 0) / 1000
                            onCommitted: value => output.saveOptions(output.settings.rateControl,
                                output.settings.preset, Math.round(value * 1000))
                        }
                        SettingText {
                            width: parent.width
                            visible: output.expanded
                            label: qsTr("Preset")
                            inputName: "outputPreset"
                            text: output.settings.preset || ""
                            placeholderText: qsTr("Encoder default")
                            onCommitted: value => output.saveOptions(output.settings.rateControl,
                                value, output.settings.keyframeMs)
                        }
                        ToolButton {
                            objectName: "destinationSettings"
                            width: parent.width
                            height: 30
                            visible: output.expanded
                            label: qsTr("Destination: %1  \u2699").arg(output.name)
                            tip: qsTr("Edit destination name, URL, and key")
                            onClicked: dialog.selectedDestinationId =
                                dialog.selectedDestinationId === output.outputId ? "" : output.outputId
                        }
                        Column {
                            width: parent.width
                            spacing: 6
                            visible: output.expanded && dialog.selectedDestinationId === output.outputId
                            SettingText {
                                width: parent.width
                                label: qsTr("Name")
                                inputName: "outputDestinationName"
                                text: output.name
                                onCommitted: value => Document.renameOutput(output.outputId, value)
                            }
                            ChoiceField {
                                width: parent.width
                                label: qsTr("Protocol")
                                labelWidth: 100
                                choices: Document.destinationKinds.map(k => k.title)
                                value: output.destinationKind.toUpperCase()
                                onCommitted: value => Document.setOutputDestination(
                                    output.outputId, value.toLowerCase(), output.url)
                            }
                            SettingText {
                                width: parent.width
                                label: qsTr("URL")
                                inputName: "outputUrl"
                                text: output.url
                                onCommitted: value => Document.setOutputDestination(
                                    output.outputId, output.destinationKind, value)
                            }
                            SettingText {
                                id: keyField
                                onVisibleChanged: if (!visible) text = ""
                                width: parent.width
                                label: qsTr("Key")
                                inputName: "outputKey"
                                echoMode: TextInput.Password
                                placeholderText: output.credentialRef
                                    ? qsTr("Saved reference - enter replacement key") : qsTr("Stream key")
                            }
                            DialogButton {
                                objectName: "saveOutputKey"
                                text: qsTr("Save key to keyring")
                                enabled: keyField.text.length > 0
                                onClicked: {
                                    Document.saveOutputKey(output.outputId, keyField.text);
                                    keyField.text = "";
                                }
                            }
                            Text {
                                width: parent.width
                                text: qsTr("Key stored in system keyring, never in project. Encoder changes affect every destination sharing this encode.")
                                wrapMode: Text.WordWrap
                                color: Theme.c.textMuted
                                font.pixelSize: 10
                            }
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
                            model: {
                                const encode = dialog.encodeOptions.find(e => e.id === encodeBlock.encodeId);
                                return encode ? encode.destinations : [];
                            }
                            delegate: ToolButton {
                                required property var modelData
                                width: content.width - 24
                                x: 12
                                height: 28
                                label: modelData.name + "  \u2699"
                                tip: qsTr("Edit destination")
                                onClicked: dialog.openDestination(modelData.id)
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
