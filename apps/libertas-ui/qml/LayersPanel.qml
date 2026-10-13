// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The selected scene's tree, flattened with indentation -- which is what a layers panel is.
///
/// The top row is the front-most layer, matching `roots[0]` in the document (ADR-0021). Reversing
/// it for display would mean every other piece of UI reverses it too, and one of them eventually
/// forgets.
Item {
    id: panel

    Text {
        anchors.centerIn: parent
        visible: Document.selectedScene === ""
        text: qsTr("No scene selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        clip: true
        visible: Document.selectedScene !== ""
        model: Document.layers
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: row

            objectName: "layerRow"

            required property int index
            required property string layerId
            required property string name
            required property string kind
            required property string sourceName
            required property int depth
            required property bool layerVisible
            required property bool locked
            required property bool isGroup
            required property int effectCount

            readonly property bool selected: Document.selectedLayer === row.layerId

            width: ListView.view.width
            height: 26
            color: row.selected ? Theme.c.accent
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                onClicked: Document.selectLayer(row.layerId)
                onDoubleClicked: {
                    rename.text = row.name;
                    rename.visible = true;
                    rename.forceActiveFocus();
                    rename.selectAll();
                }
            }

            // Visibility, first and widest, because it is the control that gets used constantly.
            Rectangle {
                id: eye

                x: 4 + row.depth * 12
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                radius: 2
                color: eyeMouse.containsMouse ? Qt.rgba(Theme.c.window.r, Theme.c.window.g,
                                                        Theme.c.window.b, 0.35)
                                              : "transparent"

                Text {
                    anchors.centerIn: parent
                    text: row.layerVisible ? "◉" : "○"
                    font.pixelSize: 12
                    color: row.selected ? Theme.c.window
                         : row.layerVisible ? Theme.c.text
                         : Theme.c.textMuted
                }

                MouseArea {
                    id: eyeMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: Document.setLayerVisible(row.layerId, !row.layerVisible)
                }
            }

            Column {
                anchors.left: eye.right
                anchors.leftMargin: 6
                anchors.right: badges.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    width: parent.width
                    visible: !rename.visible
                    text: row.name
                    font.pixelSize: 12
                    font.weight: row.isGroup ? Font.DemiBold : Font.Normal
                    color: row.selected ? Theme.c.window
                         : row.layerVisible ? Theme.c.text
                         : Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    visible: !rename.visible && row.sourceName !== ""
                    text: row.sourceName
                    font.pixelSize: 9
                    color: row.selected ? Theme.c.window : Theme.c.textMuted
                    opacity: 0.75
                    elide: Text.ElideRight
                }
            }

            TextInput {
                id: rename

                anchors.left: eye.right
                anchors.leftMargin: 6
                anchors.right: badges.left
                anchors.verticalCenter: parent.verticalCenter
                visible: false
                font.pixelSize: 12
                color: row.selected ? Theme.c.window : Theme.c.text
                selectionColor: Theme.c.accent
                selectByMouse: true

                onAccepted: {
                    visible = false;
                    if (text !== row.name)
                        Document.renameLayer(row.layerId, text);
                }
                onActiveFocusChanged: {
                    if (!activeFocus)
                        visible = false;
                }
                Keys.onEscapePressed: visible = false
            }

            Row {
                id: badges

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.kind !== "layer"
                    text: row.kind
                    font.pixelSize: 9
                    color: row.selected ? Theme.c.window : Theme.c.textMuted
                    opacity: 0.8
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.effectCount > 0
                    text: qsTr("fx %1").arg(row.effectCount)
                    font.pixelSize: 9
                    color: row.selected ? Theme.c.window : Theme.c.accent
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.locked ? "■" : "□"
                    font.pixelSize: 10
                    color: row.selected ? Theme.c.window
                         : row.locked ? Theme.c.warning
                         : Theme.c.textMuted
                    opacity: row.locked ? 1.0 : (lockMouse.containsMouse ? 0.9 : 0.35)

                    MouseArea {
                        id: lockMouse

                        anchors.fill: parent
                        anchors.margins: -4
                        hoverEnabled: true
                        onClicked: Document.setLayerLocked(row.layerId, !row.locked)
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
                tip: qsTr("Add layer")
                enabled: Document.selectedScene !== ""
                onClicked: Document.addLayer("")
            }

            ToolButton {
                label: "▣"
                tip: qsTr("Add group")
                enabled: Document.selectedScene !== ""
                onClicked: Document.addGroup("")
            }

            ToolButton {
                label: "−"
                tip: qsTr("Remove")
                enabled: Document.selectedLayer !== ""
                onClicked: Document.removeLayer(Document.selectedLayer)
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "▲"
                tip: qsTr("Move forward")
                enabled: Document.selectedLayer !== ""
                onClicked: Document.moveLayer(Document.selectedLayer, -1)
            }

            ToolButton {
                label: "▼"
                tip: qsTr("Move back")
                enabled: Document.selectedLayer !== ""
                onClicked: Document.moveLayer(Document.selectedLayer, 1)
            }
        }
    }
}
