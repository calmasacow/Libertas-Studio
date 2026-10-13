// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// What is in the selected scene: its sources, flattened with indentation.
///
/// Anything that can go into a scene is a source -- a capture card, a webcam, a browser, a flat
/// colour. They differ by *type*, not by kind, and there is no second word for any of them.
///
/// The document calls a placed source a *layer*, which is the one place the code and the screen
/// use different words, and it is deliberate: placing a source gives it a transform, effects,
/// blend and per-canvas overrides that the source itself does not have. Same thing, two amounts
/// of information. That is why `Document.layers` feeds the list while `Document.sources` feeds
/// the "existing" half of the add menu -- the first is this scene, the second is the project.
///
/// The top row is the front-most source, matching `roots[0]` in the document (ADR-0021).
/// Reversing it for display would mean every other piece of UI reverses it too, and one of them
/// eventually forgets.
Item {
    id: panel

    /// The add menu is open. Closed by picking something, or by the + again.
    property bool adding: false

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
        anchors.bottom: picker.visible ? picker.top : footer.top
        clip: true
        visible: Document.selectedScene !== ""
        model: Document.layers
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: row

            objectName: "sourceRow"

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

    // Adding a source, drawn in the panel rather than as a popup window: ADR-0015 keeps the
    // workspace to one surface, so there is nothing here for a compositor to place elsewhere.
    //
    // Two halves, which is the whole of the Inputs panel this replaced. A *new* source is one
    // the project does not have yet. An *existing* one is already configured, and picking it
    // puts a second layer on the same camera rather than opening the device twice -- which is
    // the reason sources live at project level at all.
    Rectangle {
        id: picker

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        height: visible ? Math.min(pickerColumn.height + 8, panel.height * 0.7) : 0
        visible: panel.adding
        color: Theme.c.panelHeader
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.accent
        }

        Flickable {
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            contentHeight: pickerColumn.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: pickerColumn

                width: picker.width
                spacing: 0

                Text {
                    x: 10
                    text: qsTr("New source")
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    color: Theme.c.accent
                    bottomPadding: 2
                }

                Repeater {
                    model: Document.sourceTypes

                    delegate: Rectangle {
                        id: typeRow

                        required property var modelData

                        width: pickerColumn.width
                        height: 20
                        color: typeMouse.containsMouse ? Theme.c.accent : "transparent"

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: typeRow.modelData.title
                            font.pixelSize: 11
                            color: typeMouse.containsMouse ? Theme.c.window : Theme.c.text
                        }

                        // ADR-0025: nothing it depends on can fault, so it is allowed in an
                        // emergency scene.
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            visible: typeRow.modelData.selfContained
                            text: "⛨"
                            font.pixelSize: 10
                            color: typeMouse.containsMouse ? Theme.c.window : Theme.c.good
                            opacity: 0.8
                        }

                        MouseArea {
                            id: typeMouse

                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                Document.addSourceAsLayer(typeRow.modelData.title,
                                                          typeRow.modelData.id);
                                panel.adding = false;
                            }
                        }
                    }
                }

                Item {
                    width: 1
                    height: 6
                    visible: Document.sources.rowCount() > 0
                }

                Text {
                    x: 10
                    visible: Document.sources.rowCount() > 0
                    text: qsTr("Existing source")
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                    color: Theme.c.accent
                    bottomPadding: 2
                }

                Repeater {
                    model: Document.sources

                    delegate: Rectangle {
                        id: existingRow

                        required property string sourceId
                        required property string name
                        required property string type
                        required property int useCount
                        required property bool selfContained

                        objectName: "existingSourceRow"
                        width: pickerColumn.width
                        height: 20
                        color: existingMouse.containsMouse ? Theme.c.accent : "transparent"

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.right: existingMarks.left
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            text: existingRow.name
                            font.pixelSize: 11
                            color: existingMouse.containsMouse ? Theme.c.window : Theme.c.text
                            elide: Text.ElideRight
                        }

                        Row {
                            id: existingMarks

                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 5

                            // How many layers across the project use it, which is the number
                            // that says whether removing it is safe.
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: existingRow.useCount === 0
                                    ? qsTr("unused")
                                    : qsTr("used by %1").arg(existingRow.useCount)
                                font.pixelSize: 9
                                color: existingMouse.containsMouse ? Theme.c.window
                                     : existingRow.useCount === 0 ? Theme.c.warning
                                     : Theme.c.textMuted
                                opacity: 0.85
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: existingRow.selfContained
                                text: "⛨"
                                font.pixelSize: 10
                                color: existingMouse.containsMouse ? Theme.c.window
                                                                   : Theme.c.good
                                opacity: 0.8
                            }

                            // Only offered for one nothing is using. Removing a source that
                            // layers still point at is allowed by the model -- it is a warning,
                            // not an error -- but doing it from a menu, by accident, is not.
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: existingRow.useCount === 0
                                text: "−"
                                font.pixelSize: 13
                                color: dropMouse.containsMouse ? Theme.c.error
                                     : existingMouse.containsMouse ? Theme.c.window
                                     : Theme.c.textMuted

                                MouseArea {
                                    id: dropMouse

                                    anchors.fill: parent
                                    anchors.margins: -4
                                    hoverEnabled: true
                                    onClicked: Document.removeSource(existingRow.sourceId)
                                }

                                ToolTipText {
                                    text: qsTr("Delete this source from the project")
                                    visible: dropMouse.containsMouse
                                    hoverArea: dropMouse
                                }
                            }
                        }

                        MouseArea {
                            id: existingMouse

                            anchors.fill: parent
                            anchors.rightMargin: existingMarks.width + 12
                            hoverEnabled: true
                            onClicked: {
                                Document.addExistingSourceAsLayer(existingRow.sourceId);
                                panel.adding = false;
                            }
                        }
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
                label: panel.adding ? "×" : "+"
                tip: panel.adding ? qsTr("Cancel") : qsTr("Add a source")
                enabled: Document.selectedScene !== ""
                onClicked: panel.adding = !panel.adding
            }

            // Remove sits next to add, and the group button on the far side: the two that
            // create something are not what you want either side of the one that destroys.
            ToolButton {
                label: "−"
                tip: qsTr("Remove")
                enabled: Document.selectedLayer !== ""
                onClicked: Document.removeLayer(Document.selectedLayer)
            }

            ToolButton {
                label: "▣"
                tip: qsTr("Add group")
                enabled: Document.selectedScene !== ""
                onClicked: Document.addGroup("")
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
