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
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

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
                                    hoverArea: takeMouse
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
