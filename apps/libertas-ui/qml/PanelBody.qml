// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

// Panel contents.
//
// Everything here is a placeholder, and says so. This is the Phase 1 workspace slice: the layout
// engine, the docking gesture and theme following are real, the panel contents are not yet
// wired to anything. Drawing convincing fake meters and a fake scene list would make the shell
// look finished and make it impossible to tell, later, which parts actually work.
Item {
    id: body

    required property string panelId

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelBackground
    }

    // One Loader rather than three panels with two of them hidden.
    //
    // A hidden item is still a built item: the first version instantiated all three document
    // panels in *every* panel frame, so a six-panel workspace held six scene lists and six
    // source lists, each with its own ListView over the same model. Only one was ever visible.
    Loader {
        id: panelLoader

        anchors.fill: parent
        active: sourceComponent !== null
        sourceComponent: {
            switch (body.panelId) {
            case "scenes":
                return scenesComponent;
            case "sources":
                return sourcesComponent;
            case "properties":
                return propertiesComponent;
            case "shortcuts":
                return shortcutsComponent;
            case "info":
                return infoComponent;
            case "broadcast-stats":
                return broadcastComponent;
            case "log":
                return logComponent;
            case "audio-mixer":
                return mixerComponent;
            case "controls":
                return controlsComponent;
            case "outputs":
                return outputsComponent;
            case "favourites":
                return favouritesComponent;
            default:
                return null;
            }
        }
    }

    Component {
        id: scenesComponent

        ScenesPanel {}
    }

    Component {
        id: sourcesComponent

        SourcesPanel {}
    }

    Component {
        id: propertiesComponent

        PropertiesPanel {}
    }

    Component {
        id: shortcutsComponent

        ShortcutsPanel {}
    }

    Component {
        id: infoComponent

        InfoPanel {}
    }

    Component {
        id: broadcastComponent

        BroadcastStatsPanel {}
    }

    Component {
        id: logComponent

        LogPanel {}
    }

    Component {
        id: mixerComponent

        AudioMixerPanel {}
    }

    Component {
        id: controlsComponent

        ControlsPanel {}
    }

    Component {
        id: outputsComponent

        OutputsPanel {}
    }

    Component {
        id: favouritesComponent

        FavouritesPanel {}
    }

    // The program canvas. ADR-0015 rule 2: it is a region of this surface, never a separate OS
    // window, so it cannot be lost, moved off-screen or closed by accident.
    Item {
        visible: body.panelId === "program"
        anchors.fill: parent
        anchors.margins: 10

        Rectangle {
            id: canvasFrame

            anchors.centerIn: parent
            // 16:9, letterboxed into whatever space the layout gave us.
            readonly property real fit: Math.min(parent.width / 16, parent.height / 9)
            width: Math.max(fit * 16, 1)
            height: Math.max(fit * 9, 1)
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border

            // Thirds, which is what an operator actually frames against.
            Repeater {
                model: 2

                delegate: Rectangle {
                    required property int index
                    x: canvasFrame.width * (index + 1) / 3
                    width: 1
                    height: canvasFrame.height
                    color: Theme.c.border
                    opacity: 0.35
                }
            }

            Repeater {
                model: 2

                delegate: Rectangle {
                    required property int index
                    y: canvasFrame.height * (index + 1) / 3
                    width: canvasFrame.width
                    height: 1
                    color: Theme.c.border
                    opacity: 0.35
                }
            }

            Column {
                anchors.centerIn: parent
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Program")
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("no pipeline attached")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                    opacity: 0.7
                }
            }
        }
    }

    // A panel this build has no body for. Every panel in the default layout has one now, so
    // this is reached only by a layout file naming something we do not know -- a layout saved by
    // a newer build, or one hand-edited. Asked of the Loader rather than written out as a list
    // of panel ids: the list would be the switch above copied, and the copy would be the one
    // that went stale.
    Column {
        visible: panelLoader.sourceComponent === null && body.panelId !== "" &&
                 body.panelId !== "program"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 6

        Text {
            text: Workspace.title(body.panelId)
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.c.text
        }

        Text {
            width: parent.width
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: Theme.c.textMuted
            text: qsTr("This build has nothing to draw in a panel named '%1'.")
                      .arg(body.panelId)
        }
    }

    // An empty group can exist for a moment during a drag; say so rather than draw nothing.
    Text {
        visible: body.panelId === ""
        anchors.centerIn: parent
        text: qsTr("empty")
        font.pixelSize: 11
        color: Theme.c.textMuted
        opacity: 0.5
    }
}
