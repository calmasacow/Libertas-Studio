// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Choosing what to put in a scene: types down the left, what you already have on the right.
///
/// Modelled on the dialog OBS gets right. A menu was tried first and was wrong — a menu is for a
/// short list of verbs, and this is a browse: ten types, each with its own set of existing
/// sources, and the thing you want is usually one you already made.
///
/// It is drawn inside the workspace rather than being its own window. ADR-0015 keeps this program
/// to one surface, and the operator's own complaint about OBS was dialogs and docks ending up
/// somewhere they could not be found. A modal that cannot leave the window cannot be lost behind
/// it, and on a second monitor it still appears where the application is.
///
/// Thumbnails are honest. A colour is the one source this build can draw correctly with no
/// renderer, so colours show their real swatch; everything else shows its type glyph until there
/// is a pipeline to ask. Drawing a fake camera frame would make the dialog look finished.
Item {
    id: dialog

    /// The type whose sources are on the right. Empty means the "Recently Created" view.
    property string selectedType: ""
    property string selectedSource: ""
    property bool makeVisible: true

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open() {
        dialog.selectedType = "";
        dialog.selectedSource = "";
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.closed();
    }

    function titleOf(typeId) {
        for (const t of Document.sourceTypes) {
            if (t.id === typeId)
                return t.title;
        }
        return typeId;
    }

    /// What the right-hand side is showing: every source of the chosen type, or the most
    /// recently created ones when no type is chosen.
    readonly property var shownSources: dialog.selectedType === ""
        ? Document.recentSources(24)
        : Document.sourcesOfType(dialog.selectedType)

    Keys.onEscapePressed: dialog.close()

    // Catches every click that is not on the dialog, so the workspace behind is inert while this
    // is up -- and clicking away closes it, which is what a scrim is for.
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
        width: Math.min(parent.width - 48, 1040)
        height: Math.min(parent.height - 48, 620)
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        // Swallows clicks so the scrim behind does not close the dialog when it is used.
        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        // ---- the type list -------------------------------------------------------------
        Rectangle {
            id: typeColumn

            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: bottomBar.top
            anchors.margins: 1
            width: Math.min(parent.width * 0.32, 300)
            color: Theme.c.panelHeader

            ListView {
                id: typeList

                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                clip: true
                // The list grows with every source type this build learns, so it scrolls.
                boundsBehavior: Flickable.StopAtBounds
                model: Document.sourceTypes
                header: recentHeader

                delegate: Rectangle {
                    id: typeRow

                    required property var modelData

                    readonly property bool chosen: dialog.selectedType === typeRow.modelData.id

                    objectName: "addSourceType"
                    width: typeList.width
                    height: 30
                    color: typeRow.chosen ? Theme.c.accent
                         : typeMouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"

                    Text {
                        id: typeGlyph

                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20
                        text: typeRow.modelData.icon
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        color: typeRow.chosen ? Theme.c.window : Theme.c.textMuted
                    }

                    Text {
                        anchors.left: typeGlyph.right
                        anchors.leftMargin: 8
                        anchors.right: typeSafe.left
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        text: typeRow.modelData.title
                        font.pixelSize: 12
                        color: typeRow.chosen ? Theme.c.window : Theme.c.text
                        elide: Text.ElideRight
                    }

                    // ADR-0025: nothing it depends on can fault, so it may hold a safety net.
                    Text {
                        id: typeSafe

                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        visible: typeRow.modelData.selfContained
                        text: "⛨"
                        font.pixelSize: 11
                        color: typeRow.chosen ? Theme.c.window : Theme.c.good
                        opacity: 0.8
                    }

                    MouseArea {
                        id: typeMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            dialog.selectedType = typeRow.modelData.id;
                            dialog.selectedSource = "";
                        }
                    }
                }
            }

            Component {
                id: recentHeader

                Rectangle {
                    readonly property bool chosen: dialog.selectedType === ""

                    objectName: "addSourceRecent"
                    width: typeList.width
                    height: 30
                    color: chosen ? Theme.c.accent
                         : recentMouse.containsMouse ? Theme.c.panelHeaderActive
                         : "transparent"

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Recently Created")
                        font.pixelSize: 12
                        color: parent.chosen ? Theme.c.window : Theme.c.text
                    }

                    MouseArea {
                        id: recentMouse

                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            dialog.selectedType = "";
                            dialog.selectedSource = "";
                        }
                    }
                }
            }
        }

        // ---- the heading --------------------------------------------------------------
        Text {
            id: heading

            anchors.left: typeColumn.right
            anchors.leftMargin: 16
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.top: parent.top
            anchors.topMargin: 12
            text: dialog.selectedType === ""
                ? qsTr("Recently Created")
                : dialog.titleOf(dialog.selectedType)
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.c.text
            elide: Text.ElideRight
        }

        Text {
            id: subheading

            anchors.left: heading.left
            anchors.right: heading.right
            anchors.top: heading.bottom
            anchors.topMargin: 2
            text: Document.selectedScene === ""
                ? qsTr("There is no scene to add to.")
                : qsTr("Choose what to add to this scene.")
            font.pixelSize: 11
            color: Document.selectedScene === "" ? Theme.c.warning : Theme.c.textMuted
        }

        // ---- the grid -----------------------------------------------------------------
        Rectangle {
            id: grid

            anchors.left: typeColumn.right
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.top: subheading.bottom
            anchors.topMargin: 10
            anchors.bottom: bottomBar.top
            anchors.bottomMargin: 10
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border
            radius: 2

            Flickable {
                id: gridScroll

                anchors.fill: parent
                anchors.margins: 10
                contentHeight: gridFlow.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Column {
                    id: gridFlow

                    width: gridScroll.width
                    spacing: 10

                    // "Add a new X" leads, the way it does in the dialog this is modelled on:
                    // the thing you cannot already have is the thing you came for when the list
                    // below is empty.
                    Rectangle {
                        objectName: "addNewSourceTile"
                        width: gridFlow.width
                        height: 52
                        visible: dialog.selectedType !== ""
                        radius: 2
                        color: newMouse.containsMouse ? Theme.c.panelHeaderActive : "transparent"
                        border.width: 1
                        border.color: newMouse.containsMouse ? Theme.c.accent : Theme.c.border

                        Text {
                            anchors.centerIn: parent
                            text: qsTr("+  Add a new %1").arg(dialog.titleOf(dialog.selectedType))
                            font.pixelSize: 12
                            color: newMouse.containsMouse ? Theme.c.accent : Theme.c.text
                        }

                        MouseArea {
                            id: newMouse

                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: Document.selectedScene !== ""
                            onClicked: {
                                Document.addSourceAsLayer(dialog.titleOf(dialog.selectedType),
                                                          dialog.selectedType,
                                                          dialog.makeVisible);
                                dialog.close();
                            }
                        }
                    }

                    Flow {
                        width: gridFlow.width
                        spacing: 10

                        Repeater {
                            model: dialog.shownSources

                            delegate: Rectangle {
                                id: tile

                                required property var modelData

                                readonly property bool chosen:
                                    dialog.selectedSource === tile.modelData.id

                                objectName: "existingSourceTile"
                                width: 150
                                height: 128
                                radius: 2
                                color: tile.chosen ? Theme.c.panelHeaderActive : "transparent"
                                border.width: 1
                                border.color: tile.chosen ? Theme.c.accent
                                            : tileMouse.containsMouse ? Theme.c.accent
                                            : Theme.c.border

                                // The preview. A colour is drawn as itself; everything else is
                                // its type's glyph, because this build cannot render a camera
                                // yet and a stand-in frame would be a lie about that.
                                Rectangle {
                                    id: thumb

                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 8
                                    height: 72
                                    radius: 2
                                    color: tile.modelData.swatch !== ""
                                        ? tile.modelData.swatch : Theme.c.panelHeader
                                    border.width: 1
                                    border.color: Theme.c.border

                                    Text {
                                        anchors.centerIn: parent
                                        visible: tile.modelData.swatch === ""
                                        text: tile.modelData.icon
                                        font.pixelSize: 28
                                        color: Theme.c.textMuted
                                        opacity: 0.75
                                    }
                                }

                                Text {
                                    id: tileName

                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: 8
                                    anchors.top: thumb.bottom
                                    anchors.topMargin: 6
                                    text: tile.modelData.name
                                    font.pixelSize: 11
                                    color: Theme.c.text
                                    elide: Text.ElideRight
                                }

                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.margins: 8
                                    anchors.top: tileName.bottom
                                    text: {
                                        const used = tile.modelData.useCount === 0
                                            ? qsTr("unused")
                                            : qsTr("used by %1").arg(tile.modelData.useCount);
                                        return dialog.selectedType === ""
                                            ? tile.modelData.typeTitle + " · " + used
                                            : used;
                                    }
                                    font.pixelSize: 9
                                    color: tile.modelData.useCount === 0
                                        ? Theme.c.warning : Theme.c.textMuted
                                    opacity: 0.85
                                    elide: Text.ElideRight
                                }

                                MouseArea {
                                    id: tileMouse

                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: dialog.selectedSource = tile.modelData.id
                                    onDoubleClicked: {
                                        Document.addExistingSourceAsLayer(tile.modelData.id,
                                                                          dialog.makeVisible);
                                        dialog.close();
                                    }
                                }
                            }
                        }
                    }

                    // Said plainly rather than leaving an empty box, and it names the type so it
                    // is obvious the list is filtered rather than broken.
                    Text {
                        width: gridFlow.width
                        visible: dialog.shownSources.length === 0
                        horizontalAlignment: Text.AlignHCenter
                        topPadding: 40
                        text: dialog.selectedType === ""
                            ? qsTr("Nothing has been created yet. Pick a type on the left.")
                            : qsTr("You have no %1 sources yet.")
                                  .arg(dialog.titleOf(dialog.selectedType))
                        font.pixelSize: 11
                        color: Theme.c.textMuted
                    }
                }
            }
        }

        // ---- the bottom bar -----------------------------------------------------------
        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 44
            color: Theme.c.panelHeader

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Theme.c.border
            }

            CheckLabel {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Make source visible")
                checked: dialog.makeVisible
                onToggled: dialog.makeVisible = !dialog.makeVisible
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                DialogButton {
                    objectName: "addExistingButton"
                    text: qsTr("Add existing")
                    primary: true
                    minimumWidth: 110
                    enabled: dialog.selectedSource !== "" && Document.selectedScene !== ""
                    onClicked: {
                        Document.addExistingSourceAsLayer(dialog.selectedSource,
                                                          dialog.makeVisible);
                        dialog.close();
                    }
                }

                DialogButton {
                    objectName: "closeAddSourceButton"
                    text: qsTr("Close")
                    onClicked: dialog.close()
                }
            }
        }
    }
}
