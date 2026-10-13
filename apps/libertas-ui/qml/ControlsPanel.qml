// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Program, preview and the outputs — the controls an operator reaches for during a show.
///
/// Nothing here starts a broadcast, and the banner at the bottom says so: there is no render
/// process yet. What every control does edit is the document, which is the state the renderer
/// will read when it exists — which scene is on program, which is staged on preview, what each
/// output would encode and where it would send it. So there is no fake "Go Live" button, and no
/// output can be enabled into a destination it does not have.
///
/// Program and preview are set from the scene selected in the scenes panel rather than from a
/// dropdown here. Two scenes may share a name, and a control that picks a scene by name would
/// pick the wrong one; the selection is unambiguous.
Item {
    id: panel

    Flickable {
        id: scroll

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: banner.top
        clip: true
        contentHeight: content.height
        contentWidth: width
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: content

            width: scroll.width
            spacing: 0

            Repeater {
                model: Document.decks

                delegate: Item {
                    id: deck

                    required property string deckId
                    required property string name
                    required property string canvasName
                    required property string programScene
                    required property string previewScene
                    required property string programSceneId
                    required property string previewSceneId
                    required property bool studioMode
                    required property string transition
                    required property int transitionMs
                    required property bool hasEmergencyScene

                    objectName: "deckBlock"
                    width: content.width
                    height: deckColumn.height + 12

                    Column {
                        id: deckColumn

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 6
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        spacing: 4

                        Text {
                            width: parent.width
                            text: deck.name + " · " + deck.canvasName
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            color: Theme.c.textMuted
                            elide: Text.ElideRight
                        }

                        // Program first and larger: it is the one that is already out there.
                        Rectangle {
                            width: parent.width
                            height: 30
                            radius: 2
                            color: Theme.c.panelHeader
                            border.width: 1
                            border.color: deck.programSceneId !== ""
                                ? Theme.c.error : Theme.c.border

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("PGM")
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                color: deck.programSceneId !== ""
                                    ? Theme.c.error : Theme.c.textMuted
                            }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 38
                                anchors.right: pgmSet.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: deck.programSceneId === ""
                                    ? qsTr("nothing on air") : deck.programScene
                                font.pixelSize: 12
                                color: deck.programSceneId === ""
                                    ? Theme.c.textMuted : Theme.c.text
                                elide: Text.ElideRight
                            }

                            ToolButton {
                                id: pgmSet

                                anchors.right: parent.right
                                anchors.rightMargin: 5
                                anchors.verticalCenter: parent.verticalCenter
                                label: "◉"
                                tip: qsTr("Put the selected scene on program")
                                enabled: Document.selectedScene !== "" &&
                                         Document.selectedScene !== deck.programSceneId
                                onClicked: Document.setDeckProgram(deck.deckId,
                                                                   Document.selectedScene)
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 26
                            radius: 2
                            color: Theme.c.panelHeader
                            border.width: 1
                            border.color: Theme.c.border

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("PVW")
                                font.pixelSize: 9
                                font.weight: Font.DemiBold
                                color: Theme.c.good
                            }

                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 38
                                anchors.right: pvwSet.left
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                text: deck.previewSceneId === ""
                                    ? qsTr("nothing staged") : deck.previewScene
                                font.pixelSize: 11
                                color: deck.previewSceneId === ""
                                    ? Theme.c.textMuted : Theme.c.text
                                elide: Text.ElideRight
                            }

                            ToolButton {
                                id: pvwSet

                                anchors.right: parent.right
                                anchors.rightMargin: 5
                                anchors.verticalCenter: parent.verticalCenter
                                label: "○"
                                tip: qsTr("Stage the selected scene on preview")
                                enabled: Document.selectedScene !== "" &&
                                         Document.selectedScene !== deck.previewSceneId
                                onClicked: Document.setDeckPreview(deck.deckId,
                                                                   Document.selectedScene)
                            }
                        }

                        Row {
                            width: parent.width
                            spacing: 6

                            Rectangle {
                                objectName: "takeButton"
                                width: 58
                                height: 22
                                radius: 2
                                enabled: deck.previewSceneId !== "" &&
                                         deck.previewSceneId !== deck.programSceneId
                                color: !enabled ? "transparent"
                                     : takeMouse.pressed ? Theme.c.error
                                     : takeMouse.containsMouse ? Theme.c.panelHeaderActive
                                     : Theme.c.panelHeader
                                border.width: 1
                                border.color: !enabled ? Theme.c.border
                                            : takeMouse.containsMouse ? Theme.c.error
                                            : Theme.c.border

                                Text {
                                    anchors.centerIn: parent
                                    text: qsTr("TAKE")
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                    color: parent.enabled ? Theme.c.text : Theme.c.textMuted
                                    opacity: parent.enabled ? 1.0 : 0.4
                                }

                                MouseArea {
                                    id: takeMouse

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: Document.takeDeck(deck.deckId)
                                }

                                ToolTipText {
                                    text: qsTr("Put what is on preview onto program")
                                    visible: takeMouse.containsMouse
                                    anchorItem: parent
                                }
                            }

                            CheckLabel {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Studio")
                                checked: deck.studioMode
                                onToggled: Document.setDeckStudioMode(deck.deckId,
                                                                      !deck.studioMode)
                            }
                        }

                        ChoiceField {
                            width: parent.width
                            label: qsTr("Transition")
                            choices: {
                                const out = [];
                                for (const t of Document.transitions)
                                    out.push(t.implemented ? t.title : t.title + " (no renderer)");
                                return out;
                            }
                            value: {
                                for (const t of Document.transitions) {
                                    if (t.id === deck.transition)
                                        return t.implemented
                                            ? t.title : t.title + " (no renderer)";
                                }
                                return deck.transition;
                            }
                            onCommitted: (newValue) => {
                                for (const t of Document.transitions) {
                                    const shown = t.implemented
                                        ? t.title : t.title + " (no renderer)";
                                    if (shown === newValue) {
                                        Document.setDeckTransition(deck.deckId, t.id, -1);
                                        return;
                                    }
                                }
                            }
                        }

                        NumberField {
                            width: parent.width
                            label: qsTr("Duration")
                            decimals: 0
                            value: deck.transitionMs
                            onCommitted: (newValue) => Document.setDeckTransition(
                                deck.deckId, "", Math.max(0, Math.round(newValue)))
                        }

                        // ADR-0025: a canvas with no safety net is not a broken document, it is
                        // a pre-flight question. This is where the answer belongs.
                        Text {
                            width: parent.width
                            visible: !deck.hasEmergencyScene
                            text: qsTr("⚠ %1 has no emergency scene").arg(deck.canvasName)
                            font.pixelSize: 9
                            color: Theme.c.warning
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            Text {
                width: content.width
                visible: Document.decks.rowCount() === 0
                padding: 10
                text: qsTr("No deck. A deck is one program path for one canvas.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }

            // ---- outputs ----
            Rectangle {
                width: content.width
                height: 1
                color: Theme.c.border
            }

            Item {
                width: content.width
                height: 22

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("OUTPUTS")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                ToolButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    label: "+"
                    tip: qsTr("Add an output")
                    onClicked: Document.addOutput(qsTr("Output"))
                }
            }

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

    // Where a "Go Live" button would be, if there were anything behind it.
    Rectangle {
        id: banner

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

        Text {
            anchors.centerIn: parent
            text: qsTr("no render process — nothing is encoding")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
        }
    }
}
