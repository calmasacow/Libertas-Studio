// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Which Main scenes bring this scene up.
///
/// The direction matters and is easy to get backwards. This is opened from a scene on a
/// *secondary* canvas, and it does not ask what that scene cues -- it asks which scenes on Main
/// cue it. Several Main scenes selecting one vertical scene is the ordinary case: a show where
/// three landscape scenes all want the same vertical framing (ADR-0027).
///
/// Main itself has no link control anywhere. Linking is purely a secondary canvas's function,
/// and the command layer refuses a link whose source is not on Main, so this dialog only ever
/// lists Main's scenes.
///
/// "Link" is the only commit point. "Clear links" empties the tick-list and writes nothing, so
/// closing this any other way -- Escape, the scrim, the window button -- always leaves the
/// project exactly as it was found, and clearing then relinking is one undo step rather than
/// two.
Item {
    id: dialog

    property string sceneId: ""
    property string sceneName: ""
    /// The ids currently ticked. Local until "Link" is pressed.
    property var ticked: []
    /// The rows as the document reported them when this opened: id, name, ticked, takenBy.
    property var rows: []

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open(id, name) {
        dialog.sceneId = id;
        dialog.sceneName = name;
        dialog.rows = Document.mainScenesForLink(id);
        const start = [];
        for (const r of dialog.rows) {
            if (r.ticked)
                start.push(r.id);
        }
        dialog.ticked = start;
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.closed();
    }

    function isTicked(id) {
        return dialog.ticked.indexOf(id) !== -1;
    }

    function toggle(id) {
        const next = dialog.ticked.slice();
        const at = next.indexOf(id);
        if (at === -1)
            next.push(id);
        else
            next.splice(at, 1);
        dialog.ticked = next;
    }

    /// The scenes that would lose their link if this were committed now. Shown before the
    /// operator presses Link, not reported afterwards: a Main scene brings up at most one scene
    /// per canvas, so ticking one that is already spoken for moves it.
    readonly property var moving: {
        const out = [];
        for (const r of dialog.rows) {
            if (r.takenBy !== "" && dialog.isTicked(r.id))
                out.push(r.name + " → " + r.takenBy);
        }
        return out;
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
        width: Math.min(parent.width - 48, 420)
        // Fits the list rather than standing at a fixed height. A show with three scenes on
        // Main got a half-empty 480px box, which reads as a panel that failed to load its
        // contents.
        height: Math.min(parent.height - 48, 480,
                         116 + dialog.rows.length * 28 + (warning.visible ? warning.height : 0))
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

            ChainIcon {
                id: titleChain

                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.c.accent
                strong: true
            }

            Column {
                anchors.left: titleChain.right
                anchors.leftMargin: 8
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0

                Text {
                    width: parent.width
                    text: qsTr("Scenes that bring up “%1”").arg(dialog.sceneName)
                    font.pixelSize: 11
                    font.bold: true
                    color: Theme.c.text
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: qsTr("Taking one of these on Main switches this canvas.")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }
            }
        }

        ListView {
            id: list

            objectName: "sceneLinkList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleBar.bottom
            anchors.bottom: warning.top
            anchors.margins: 1
            anchors.topMargin: 4
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
            // because Flickable drops it along with the drag.
            interactive: false

            NoDragScroll {}
            model: dialog.rows

            delegate: Rectangle {
                id: linkRow

                required property var modelData

                readonly property bool on: dialog.isTicked(linkRow.modelData.id)

                objectName: "sceneLinkRow"
                width: list.width
                height: linkRow.modelData.takenBy === "" ? 26 : 38
                color: rowMouse.containsMouse ? Theme.c.panelHeader : "transparent"

                Rectangle {
                    id: box

                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.top: parent.top
                    anchors.topMargin: 6
                    width: 13
                    height: 13
                    radius: 2
                    color: linkRow.on ? Theme.c.accent : "transparent"
                    border.width: 1
                    border.color: linkRow.on ? Theme.c.accent : Theme.c.border

                    Text {
                        anchors.centerIn: parent
                        visible: linkRow.on
                        text: "✓"
                        font.pixelSize: 10
                        color: Theme.c.window
                    }
                }

                Column {
                    anchors.left: box.right
                    anchors.leftMargin: 8
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.top: parent.top
                    anchors.topMargin: 5
                    spacing: 1

                    Text {
                        width: parent.width
                        text: linkRow.modelData.name
                        font.pixelSize: 11
                        color: Theme.c.text
                        elide: Text.ElideRight
                    }

                    // Said before the commit, not reported after it. A Main scene brings up at
                    // most one scene per canvas, so ticking one that is spoken for moves it.
                    Text {
                        width: parent.width
                        visible: linkRow.modelData.takenBy !== ""
                        text: linkRow.on
                              ? qsTr("moves here from “%1”").arg(linkRow.modelData.takenBy)
                              : qsTr("currently brings up “%1”").arg(linkRow.modelData.takenBy)
                        font.pixelSize: 9
                        color: linkRow.on ? Theme.c.warning : Theme.c.textMuted
                        elide: Text.ElideRight
                    }
                }

                MouseArea {
                    id: rowMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: dialog.toggle(linkRow.modelData.id)
                }
            }

            footer: Text {
                width: list.width
                visible: dialog.rows.length === 0
                padding: 10
                text: qsTr("Main has no scenes to link to yet.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                wrapMode: Text.WordWrap
            }
        }

        Text {
            id: warning

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: bottomBar.top
            anchors.leftMargin: 11
            anchors.rightMargin: 11
            visible: dialog.moving.length > 0
            height: visible ? implicitHeight + 8 : 0
            text: qsTr("⚠ Linking moves: %1").arg(dialog.moving.join(", "))
            font.pixelSize: 9
            color: Theme.c.warning
            wrapMode: Text.WordWrap
        }

        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 40
            color: Theme.c.panelHeader

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                DialogButton {
                    objectName: "clearLinks"
                    text: qsTr("Clear links")
                    enabled: dialog.ticked.length > 0
                    // Unticks only. Nothing is written until Link, so closing after this
                    // changes nothing -- and clearing then relinking is one undo step.
                    onClicked: dialog.ticked = []
                }

                DialogButton {
                    objectName: "commitLinks"
                    text: qsTr("Link")
                    primary: true
                    onClicked: {
                        Document.commitSceneLinks(dialog.sceneId, dialog.ticked);
                        dialog.close();
                    }
                }
            }
        }
    }
}
