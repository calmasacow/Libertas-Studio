// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Project-level source instances — the things feeding the show.
///
/// Separate from the layers panel on purpose. A source lives at project level so that two layers,
/// or two scenes, can show one camera without opening the device twice; a panel that presented
/// them as belonging to a scene would make that impossible to see. The count beside each one is
/// how many layers across the whole project are bound to it, which is what says whether removing
/// it is safe.
Item {
    id: panel

    property bool adding: false

    ListView {
        id: list

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: typePicker.visible ? typePicker.top : footer.top
        clip: true
        model: Document.sources
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: row

            required property string sourceId
            required property string name
            required property string type
            required property int useCount
            required property bool selfContained

            objectName: "inputRow"
            width: ListView.view.width
            height: 28
            color: mouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                onDoubleClicked: {
                    rename.text = row.name;
                    rename.visible = true;
                    rename.forceActiveFocus();
                    rename.selectAll();
                }
            }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: actions.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    width: parent.width
                    visible: !rename.visible
                    text: row.name
                    font.pixelSize: 12
                    color: Theme.c.text
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    visible: !rename.visible
                    text: row.useCount === 0
                        ? qsTr("%1 · unused").arg(row.type)
                        : qsTr("%1 · used by %2").arg(row.type).arg(row.useCount)
                    font.pixelSize: 9
                    color: row.useCount === 0 ? Theme.c.textMuted : Theme.c.accent
                    opacity: row.useCount === 0 ? 0.7 : 1.0
                    elide: Text.ElideRight
                }
            }

            TextInput {
                id: rename

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: actions.left
                anchors.verticalCenter: parent.verticalCenter
                visible: false
                font.pixelSize: 12
                color: Theme.c.text
                selectionColor: Theme.c.accent
                selectByMouse: true

                onAccepted: {
                    visible = false;
                    if (text !== row.name)
                        Document.renameSource(row.sourceId, text);
                }
                onActiveFocusChanged: {
                    if (!activeFocus)
                        visible = false;
                }
                Keys.onEscapePressed: visible = false
            }

            Row {
                id: actions

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // ADR-0025: safe in an emergency scene, meaning nothing it depends on can fault.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.selfContained
                    text: "⛨"
                    font.pixelSize: 11
                    color: Theme.c.good
                    opacity: 0.8

                    ToolTipText {
                        text: qsTr("Safe to use in an emergency scene")
                        visible: safeHover.containsMouse
                        anchorItem: parent
                    }

                    MouseArea {
                        id: safeHover

                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                    }
                }

                ToolButton {
                    label: "→"
                    tip: qsTr("Bind the selected layer to this input")
                    enabled: Document.selectedLayer !== ""
                    onClicked: Document.bindSelectedLayer(row.sourceId)
                }
            }
        }
    }

    // Choosing a type is the whole of creating an input, so it is a list rather than a dialog.
    Rectangle {
        id: typePicker

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        height: visible ? Math.min(typeList.contentHeight + 22, 200) : 0
        visible: panel.adding
        color: Theme.c.panelHeader

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.accent
        }

        Text {
            id: pickerCaption

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.top: parent.top
            anchors.topMargin: 4
            text: qsTr("New input")
            font.pixelSize: 10
            font.weight: Font.DemiBold
            color: Theme.c.accent
        }

        ListView {
            id: typeList

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: pickerCaption.bottom
            anchors.topMargin: 2
            anchors.bottom: parent.bottom
            clip: true
            model: Document.sourceTypes
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: typeRow

                required property var modelData

                width: typeList.width
                height: 20
                color: typeMouse.containsMouse ? Theme.c.accent : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: typeRow.modelData.title
                    font.pixelSize: 11
                    color: typeMouse.containsMouse ? Theme.c.window : Theme.c.text
                }

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
                        Document.addSource(typeRow.modelData.title, typeRow.modelData.id);
                        panel.adding = false;
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
                tip: panel.adding ? qsTr("Cancel") : qsTr("Add input")
                onClicked: panel.adding = !panel.adding
            }
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "⊘"
            tip: qsTr("Unbind the selected layer")
            enabled: Document.selectedLayer !== ""
            onClicked: Document.bindSelectedLayer("")
        }
    }
}
