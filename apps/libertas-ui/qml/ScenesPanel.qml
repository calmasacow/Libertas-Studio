// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

Item {
    id: panel

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        model: Document.scenes
        currentIndex: -1
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: row

            // The self-check looks for these by name, so that "the model has
            // three scenes" and "three rows reached the screen" stay different
            // claims.
            objectName: "sceneRow"

            required property int index
            required property string sceneId
            required property string name
            required property int nodeCount
            required property bool isProgram
            required property bool isPreview
            required property bool isEmergency
            required property bool isFavourite

            readonly property bool selected: Document.selectedScene === row.sceneId

            width: ListView.view.width
            height: 24
            color: row.selected ? Theme.c.accent
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                onClicked: Document.selectScene(row.sceneId)
                onDoubleClicked: {
                    rename.text = row.name;
                    rename.visible = true;
                    rename.forceActiveFocus();
                    rename.selectAll();
                }
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: markers.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !rename.visible
                    text: row.name
                    font.pixelSize: 12
                    color: row.selected ? Theme.c.window : Theme.c.text
                    elide: Text.ElideRight
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !rename.visible
                    text: row.nodeCount === 0 ? qsTr("empty") : row.nodeCount
                    font.pixelSize: 10
                    color: row.selected ? Theme.c.window : Theme.c.textMuted
                    opacity: 0.7
                }
            }

            TextInput {
                id: rename

                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.right: markers.left
                anchors.verticalCenter: parent.verticalCenter
                visible: false
                font.pixelSize: 12
                color: row.selected ? Theme.c.window : Theme.c.text
                selectionColor: Theme.c.accent
                selectByMouse: true

                onAccepted: {
                    visible = false;
                    if (text !== row.name)
                        Document.renameScene(row.sceneId, text);
                }
                onActiveFocusChanged: {
                    if (!activeFocus)
                        visible = false;
                }
                Keys.onEscapePressed: visible = false
            }

            Row {
                id: markers

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // Shown filled when it is a favourite and faintly when it is not, so the way to
                // make one is discoverable by hovering the row rather than by being told.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isFavourite || mouse.containsMouse
                    text: row.isFavourite ? "★" : "☆"
                    font.pixelSize: 12
                    color: row.isFavourite
                        ? (row.selected ? Theme.c.window : Theme.c.warning)
                        : (row.selected ? Theme.c.window : Theme.c.textMuted)
                    opacity: row.isFavourite ? 1.0 : (starMouse.containsMouse ? 0.9 : 0.45)

                    MouseArea {
                        id: starMouse

                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        onClicked: Document.setSceneFavourite(row.sceneId, !row.isFavourite)
                    }

                    ToolTipText {
                        text: row.isFavourite
                            ? qsTr("Remove from favourites")
                            : qsTr("Add to favourites")
                        visible: starMouse.containsMouse
                        hoverArea: starMouse
                    }
                }

                // On air. Named rather than a coloured dot, because which dot means what is
                // exactly the thing nobody remembers at the moment it matters.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isProgram
                    width: liveLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: Theme.c.error

                    Text {
                        id: liveLabel

                        anchors.centerIn: parent
                        text: qsTr("LIVE")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.window
                    }
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isPreview
                    width: previewLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: Theme.c.good

                    Text {
                        id: previewLabel

                        anchors.centerIn: parent
                        text: qsTr("PREV")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.window
                    }
                }

                // ADR-0025: the safety net is marked, because losing it unnoticed is that
                // decision's own failure arriving by another route.
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.isEmergency
                    width: safeLabel.implicitWidth + 8
                    height: 14
                    radius: 2
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.c.warning

                    Text {
                        id: safeLabel

                        anchors.centerIn: parent
                        text: qsTr("SAFE")
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.warning
                    }
                }
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
                tip: qsTr("Add scene")
                onClicked: Document.addScene("")
            }

            ToolButton {
                label: "−"
                tip: qsTr("Remove scene")
                enabled: Document.selectedScene !== ""
                onClicked: Document.removeScene(Document.selectedScene)
            }

            ToolButton {
                label: "⧉"
                tip: qsTr("Duplicate scene")
                enabled: Document.selectedScene !== ""
                onClicked: Document.duplicateScene(Document.selectedScene)
            }
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "⚠"
            tip: qsTr("Use as the emergency scene for this canvas")
            enabled: Document.selectedScene !== ""
            onClicked: Document.setEmergencyScene(Document.selectedScene)
        }
    }
}
