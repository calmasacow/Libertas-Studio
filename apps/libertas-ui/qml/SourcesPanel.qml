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
/// Each row is a *source node* -- a source placed in this scene -- or a group of them. Which of
/// the two "source" things is which is explained once, in `ldoc/node.hpp` at `SourceData`, where
/// the types meet. Here it is enough that `Document.nodes` feeds the list while
/// `Document.sources` feeds the "existing" half of the add menu -- the first is this scene, the
/// second is the project.
///
/// The top row is the front-most source, matching `roots[0]` in the document (ADR-0021).
/// Reversing it for display would mean every other piece of UI reverses it too, and one of them
/// eventually forgets.
Item {
    id: panel

    /// Which canvas's selected scene this tree belongs to. Empty means the first canvas --
    /// the plain "sources" panel id. Titled "Sources: Vertical" in the tab, so two canvases
    /// can be worked on side by side rather than one at a time (ADR-0027).
    property string canvasId: ""

    /// Make this panel's canvas the one the shell is working on, before doing something to
    /// it. The node commands act on "the selected node of the current canvas's scene", so a
    /// second Sources panel has to say which canvas it means before it adds or moves
    /// anything -- otherwise the + on "Sources: Vertical" would quietly build Main's scene.
    function use() {
        Document.selectCanvas(panel.canvasId);
    }

    objectName: "sourcesPanel"

    /// The add menu is open. Closed by picking something, or by the + again.
    property bool adding: false

    /// The node whose name is being typed over, or empty. Held here rather than in the row so
    /// that something outside the row -- F2 -- can start a rename, and so that two rows cannot
    /// both be in edit at once.
    property string renaming: ""

    /// Open a source's settings. Takes the source id rather than the node's, because that is
    /// what is being configured: one camera in six scenes has one set of settings, and editing
    /// it from any of the six is editing the same thing.
    function configure(sourceId) {
        if (sourceId !== "")
            settingsDialog.open(sourceId);
    }

    // Renaming has to stay reachable now that double-click opens the settings instead. F2 is
    // where every file manager and every NLE put it, and it acts on the selected row wherever
    // the pointer is.
    Shortcut {
        sequence: "F2"
        enabled: Document.selectedNode !== ""
        onActivated: panel.renaming = Document.selectedNode
    }

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
        model: Document.nodesFor(panel.canvasId)
        boundsBehavior: Flickable.StopAtBounds

        delegate: Rectangle {
            id: row

            objectName: "sourceRow"

            required property int index
            required property string nodeId
            required property string name
            required property string kind
            required property string sourceName
            required property string sourceId
            required property int depth
            required property bool nodeVisible
            required property bool locked
            required property bool isGroup
            required property int effectCount

            readonly property bool selected: Document.selectedNode === row.nodeId
            readonly property bool editing: panel.renaming === row.nodeId

            width: ListView.view.width
            height: 26
            color: row.selected ? Theme.c.accent
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: (ev) => {
                    Document.selectNodeOn(panel.canvasId, row.nodeId);
                    if (ev.button !== Qt.RightButton)
                        return;
                    const at = mapToItem(nodeMenu, ev.x, ev.y);
                    const rows = [{
                        text: qsTr("Copy"),
                        checked: false,
                        action: () => Document.copySelectedNode()
                    }];
                    // Pasting a source into a scene cannot miss the canvas in a way worth
                    // warning about the way a whole scene can -- it is one thing and the
                    // operator can see where it landed -- so this goes straight through.
                    if (Document.pasteDescription() !== "") {
                        rows.push({
                            text: Document.pasteDescription(),
                            checked: false,
                            action: () => Document.paste()
                        });
                    }
                    nodeMenu.popup(at.x, at.y, rows);
                }
                // Double-click opens the settings for what the row is bound to, which is what
                // OBS does and what the operator asked for. A group, or a source node bound to
                // nothing, has no settings to open -- so there it still starts a rename, which
                // is the only thing a double-click could usefully mean on one.
                onDoubleClicked: {
                    Document.selectNodeOn(panel.canvasId, row.nodeId);
                    if (row.sourceId !== "")
                        panel.configure(row.sourceId);
                    else
                        panel.renaming = row.nodeId;
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
                    text: row.nodeVisible ? "◉" : "○"
                    font.pixelSize: 12
                    color: row.selected ? Theme.c.window
                         : row.nodeVisible ? Theme.c.text
                         : Theme.c.textMuted
                }

                MouseArea {
                    id: eyeMouse

                    anchors.fill: parent
                    hoverEnabled: true
                        onClicked: {
                        panel.use();
                        Document.setNodeVisible(row.nodeId, !row.nodeVisible);
                    }
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
                    visible: !row.editing
                    text: row.name
                    font.pixelSize: 12
                    font.weight: row.isGroup ? Font.DemiBold : Font.Normal
                    color: row.selected ? Theme.c.window
                         : row.nodeVisible ? Theme.c.text
                         : Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    visible: !row.editing && row.sourceName !== ""
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
                visible: row.editing
                font.pixelSize: 12
                color: row.selected ? Theme.c.window : Theme.c.text
                selectionColor: Theme.c.accent
                selectByMouse: true

                onVisibleChanged: {
                    if (visible) {
                        text = row.name;
                        forceActiveFocus();
                        selectAll();
                    }
                }
                onAccepted: {
                    panel.renaming = "";
                    if (text !== row.name)
                        Document.renameNode(row.nodeId, text);
                }
                onActiveFocusChanged: {
                    if (!activeFocus && row.editing)
                        panel.renaming = "";
                }
                Keys.onEscapePressed: panel.renaming = ""
            }

            Row {
                id: badges

                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                spacing: 5

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: row.kind !== "source"
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
                        onClicked: Document.setNodeLocked(row.nodeId, !row.locked)
                    }
                }
            }
        }
    }

    // Choosing what to add is a browse, not a menu: ten types, each with its own set of
    // sources you already made. So it is the dialog in AddSourceDialog.qml.
    //
    // Parented to the window's content item, like ToolTipText, so it is not clipped by this
    // panel -- and still inside the one surface, so ADR-0015 holds and it cannot end up on
    // another monitor or behind the application.
    AddSourceDialog {
        id: addDialog

        objectName: "addSourceDialog"
        parent: panel.Window.contentItem
        onClosed: panel.adding = false
    }

    StripMenu {
        id: nodeMenu

        objectName: "nodeMenu"
        parent: panel.Window.contentItem
        z: 40
    }

    // The gear's panel, parented the same way and for the same reasons.
    SourceSettingsDialog {
        id: settingsDialog

        objectName: "sourceSettingsDialog"
        parent: panel.Window.contentItem
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
                tip: qsTr("Add a source")
                enabled: Document.selectedSceneOn(panel.canvasId) !== ""
                onClicked: {
                    panel.use();
                    panel.adding = true;
                    addDialog.open();
                }
            }

            // Remove sits next to add, and the group button on the far side: the two that
            // create something are not what you want either side of the one that destroys.
            ToolButton {
                label: "−"
                tip: qsTr("Remove")
                enabled: Document.selectedNode !== ""
                onClicked: {
                        panel.use();
                        Document.removeNode(Document.selectedNode);
                    }
            }

            ToolButton {
                label: "▣"
                tip: qsTr("Add group")
                enabled: Document.selectedScene !== ""
                onClicked: {
                        panel.use();
                        Document.addGroup("");
                    }
            }

            // The gear, where OBS puts it: what the selected row is *bound to*, not the row.
            // Disabled on a group and on a source node bound to nothing, because neither has
            // anything to configure, and the tip says which it is rather than leaving a dead
            // button to be poked at.
            ToolButton {
                objectName: "sourceSettingsButton"
                label: "⚙"
                tip: Document.selectedNode === "" ? qsTr("Settings")
                   : Document.nodeProperties.sourceId === ""
                     ? qsTr("This has no source to configure")
                     : qsTr("Settings for %1").arg(Document.nodeProperties.name)
                enabled: Document.selectedNode !== "" &&
                         Document.nodeProperties.sourceId !== ""
                onClicked: panel.configure(Document.nodeProperties.sourceId)
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
                enabled: Document.selectedNode !== ""
                onClicked: {
                        panel.use();
                        Document.moveNode(Document.selectedNode, -1);
                    }
            }

            ToolButton {
                label: "▼"
                tip: qsTr("Move back")
                enabled: Document.selectedNode !== ""
                onClicked: {
                        panel.use();
                        Document.moveNode(Document.selectedNode, 1);
                    }
            }
        }
    }
}
