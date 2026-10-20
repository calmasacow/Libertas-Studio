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

    /// Dragging a row, the way a layers panel does (ADR-0038). `dragNode` is what was picked
    /// up, `dropTarget` and `dropZone` are where it would land -- "above", "below" or "into".
    /// Held here rather than in the row, because the row under the pointer is not the row that
    /// was pressed.
    property string dragNode: ""
    property string dropTarget: ""
    property string dropZone: ""
    readonly property bool dragging: panel.dragNode !== ""

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

    // The canvas menu has no row to type into. It asks, and the list does the rename.
    Connections {
        target: Document
        function onRenameRequested(nodeId) {
            panel.renaming = nodeId;
        }
    }

    Text {
        anchors.centerIn: parent
        visible: Document.selectedScene === ""
        text: qsTr("No scene selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    /// Which row the pointer is over, and which third of it -- the three zones a layers panel
    /// has: above, into, below. A row that is not a group has no middle third, because there
    /// is nothing to go inside; its whole height splits in two (ADR-0038).
    function updateDrop(yInList) {
        const y = yInList + list.contentY;
        const at = Math.floor(y / 26);
        if (at < 0 || at >= list.count) {
            panel.dropTarget = "";
            return;
        }
        const hit = Document.nodeAtRow(at);
        if (hit.id === undefined) {
            panel.dropTarget = "";
            return;
        }
        const target = hit.id;
        const isGroup = hit.isGroup;
        const within = y - at * 26;
        let zone;
        if (isGroup)
            zone = within < 7 ? "above" : within > 19 ? "below" : "into";
        else
            zone = within < 13 ? "above" : "below";

        if (!Document.canDropAt(target, zone)) {
            panel.dropTarget = "";
            return;
        }
        panel.dropTarget = target;
        panel.dropZone = zone;
    }

    function endDrag() {
        panel.dragNode = "";
        panel.dropTarget = "";
        panel.dropZone = "";
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
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        delegate: Rectangle {
            id: row

            objectName: "sourceRow"

            required property int index
            required property string nodeId
            required property string name
            required property string kind
            required property string sourceType
            required property string sourceName
            required property string sourceId
            required property int depth
            required property bool nodeVisible
            required property bool locked
            required property bool isGroup
            required property int effectCount

            readonly property string displayName: row.sourceName || row.name

            readonly property bool selected: Document.selectedNode === row.nodeId
            /// Selected, but not the one panels are following. Drawn a shade down from the
            /// leader so a range reads as a range and it is still obvious which node the
            /// Properties panel would be showing if one were (ADR-0037).
            readonly property bool alsoSelected: !row.selected &&
                                                 Document.selectedNodes.indexOf(row.nodeId) >= 0
            readonly property bool editing: panel.renaming === row.nodeId

            width: ListView.view.width
            height: row.sourceName !== "" ? 36 : 26
            readonly property bool dropInto: panel.dropTarget === row.nodeId &&
                                             panel.dropZone === "into"

            color: row.dropInto ? Theme.c.accent
                 : row.selected ? Theme.c.accent
                 : row.alsoSelected ? Theme.c.panelHeaderActive
                 : mouse.containsMouse ? Theme.c.panelHeaderActive
                 : "transparent"
            // A group about to receive the drop is outlined rather than only tinted, so
            // "inside this" cannot be mistaken for "selected".
            border.width: row.dropInto ? 1 : 0
            border.color: Theme.c.text
            // The rows being carried fade, which is what says they are in flight rather than
            // still where they were.
            opacity: panel.dragging && Document.selectedNodes.indexOf(row.nodeId) >= 0 ? 0.4
                   : row.alsoSelected ? 0.85
                   : 1.0

            MouseArea {
                id: mouse

                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton

                /// Where the press landed, so a drag can start only after the pointer has
                /// actually moved. Without that every click would begin a drag and the panel
                /// would feel like it was catching on things.
                property real pressY: -1
                property real pressX: -1

                onPressed: (ev) => {
                    mouse.pressX = ev.x;
                    mouse.pressY = ev.y;
                }

                onPositionChanged: (ev) => {
                    if (ev.buttons !== Qt.LeftButton || mouse.pressY < 0)
                        return;
                    if (!panel.dragging) {
                        // Four pixels: enough that a click with a shaky hand is still a
                        // click, small enough that a deliberate drag starts at once.
                        if (Math.abs(ev.y - mouse.pressY) < 4 &&
                            Math.abs(ev.x - mouse.pressX) < 4)
                            return;
                        // Dragging something outside the selection takes just that one, which
                        // is what a layers panel does -- the selection is not a cargo net.
                        if (Document.selectedNodes.indexOf(row.nodeId) < 0)
                            Document.selectNodeOn(panel.canvasId, row.nodeId);
                        panel.dragNode = row.nodeId;
                    }
                    panel.updateDrop(mapToItem(list, ev.x, ev.y).y);
                }

                onReleased: {
                    mouse.pressY = -1;
                    if (!panel.dragging)
                        return;
                    if (panel.dropTarget !== "")
                        Document.dropSelectedAt(panel.dropTarget, panel.dropZone);
                    panel.endDrag();
                }

                onCanceled: {
                    mouse.pressY = -1;
                    panel.endDrag();
                }

                onClicked: (ev) => {
                    // Ctrl toggles one, Shift takes the run between the anchor and here --
                    // what every other list does. A right-click never changes the selection
                    // when it lands on something already in it, so the menu can act on all
                    // of them (ADR-0037).
                    if (ev.button === Qt.RightButton) {
                        if (Document.selectedNodes.indexOf(row.nodeId) < 0)
                            Document.selectNodeOn(panel.canvasId, row.nodeId);
                    } else {
                        Document.selectNodeExtended(panel.canvasId, row.nodeId,
                                                    (ev.modifiers & Qt.ControlModifier) !== 0,
                                                    (ev.modifiers & Qt.ShiftModifier) !== 0);
                    }
                    if (ev.button !== Qt.RightButton)
                        return;
                    const at = mapToItem(nodeMenu, ev.x, ev.y);
                    const rows = [{
                        text: Document.selectedNodes.length > 1
                              ? qsTr("Copy %1 sources").arg(Document.selectedNodes.length)
                              : qsTr("Copy"),
                        checked: false,
                        action: () => Document.copySelectedNodes()
                    }];
                    // Rename, which was double-click and F2 only. A right-click is where
                    // people look for it.
                    // Delete, where a right-click menu is expected to have it, and acting
                    // on the whole selection like the footer's button.
                    rows.push({
                        text: Document.selectedNodes.length > 1
                              ? qsTr("Remove %1 sources").arg(Document.selectedNodes.length)
                              : qsTr("Remove from scene"),
                        checked: false,
                        action: () => Document.removeSelectedNodes()
                    });
                    if (Document.selectedNodes.length <= 1) {
                        rows.push({
                            text: qsTr("Rename"),
                            checked: false,
                            action: () => {
                                panel.renaming = row.nodeId;
                            }
                        });
                    }
                    // Moving into a group is a drag now, not a menu of every group in the
                    // scene -- that list grows with the project and the gesture people reach
                    // for is the drag (ADR-0038). Out of a group stays, because there is
                    // nothing above the top level to drop onto.
                    if (Document.parentOfNode(row.nodeId) !== "") {
                        rows.push({
                            text: qsTr("Move out of group"),
                            checked: false,
                            action: () => Document.moveSelectedIntoGroup("")
                        });
                    }
                    // Taking a group apart, beside the gesture that makes one.
                    if (row.isGroup) {
                        rows.push({
                            text: qsTr("Ungroup"),
                            checked: false,
                            action: () => Document.ungroupNode(row.nodeId)
                        });
                    }
                    // Only with something to group. One node in a group of its own is a
                    // legitimate thing to want, so this appears for one as well as for six.
                    if (Document.selectedNodes.length > 0) {
                        rows.push({
                            text: Document.selectedNodes.length > 1
                                  ? qsTr("Group %1 sources").arg(
                                        Document.selectedNodes.length)
                                  : qsTr("Group"),
                            checked: false,
                            action: () => Document.groupSelectedNodes()
                        });
                    }
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
                    objectName: "sourcePrimaryLabel"
                    text: row.displayName
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
                    objectName: "sourceTypeLabel"
                    text: row.sourceType
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
                        text = row.displayName;
                        forceActiveFocus();
                        selectAll();
                    }
                }
                onAccepted: {
                    panel.renaming = "";
                    if (text !== row.displayName) {
                        if (row.sourceId !== "")
                            Document.renameSource(row.sourceId, text);
                        else
                            Document.renameNode(row.nodeId, text);
                    }
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
        onCreated: sourceId => panel.configure(sourceId)
    }

    /// Where the drop would land, drawn between rows the way a layers panel does. Only for
    /// above and below -- "into" is shown by outlining the group itself, because a line at
    /// the top of a group would be indistinguishable from "above it" (ADR-0038).
    Rectangle {
        objectName: "dropLine"
        parent: list
        visible: panel.dragging && panel.dropTarget !== "" && panel.dropZone !== "into"
        x: 0
        width: list.width
        height: 2
        color: Theme.c.accent
        z: 10
        y: {
            if (!visible)
                return 0;
            const at = Document.rowOfNode(panel.dropTarget);
            if (at < 0)
                return 0;
            const top = at * 26 - list.contentY;
            return panel.dropZone === "below" ? top + 26 - 1 : top - 1;
        }
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
                tip: Document.selectedNodes.length > 1
                     ? qsTr("Remove %1 sources").arg(Document.selectedNodes.length)
                     : qsTr("Remove")
                enabled: Document.selectedNode !== ""
                onClicked: {
                        panel.use();
                        Document.removeSelectedNodes();
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
