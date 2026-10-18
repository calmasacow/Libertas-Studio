// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// Selection on the program picture: drag the source, or a handle to resize it.
///
/// The rectangle is the one the compositor paints, for whichever source is selected. A handle
/// changes scale, and position so the opposite edge stays put, unless Alt is held: then the
/// center stays and both sides move. Shift keeps the aspect ratio. Alt and Shift together do
/// both. The layout box is a different control. The whole press is one undo step.
Item {
    id: overlay

    required property string canvasId
    required property real canvasWidth
    required property real canvasHeight

    objectName: "programOverlay"
    anchors.fill: parent

    property bool dragging: false
    property var hold: null
    property string edges: ""
    property real originX: 0
    property real originY: 0
    property real liveX: 0
    property real liveY: 0
    property real liveW: 0
    property real liveH: 0
    property int cursorShape: Qt.ArrowCursor

    readonly property var handleEdges: ["lt", "t", "rt", "l", "r", "lb", "b", "rb"]

    /// Follows the document, except while a drag is in hand: the outline tracks the pointer,
    /// and the picture follows the commands.
    readonly property var placement: {
        void Document.revision;
        void Document.selectedNode;
        if (overlay.dragging)
            return overlay.hold;
        if (Document.selectedNode === "")
            return null;
        const rect = Document.placedRect(overlay.canvasId, Document.selectedNode);
        return rect && rect.width > 0 ? rect : null;
    }

    readonly property var shownRect: overlay.dragging
            ? {x: overlay.liveX, y: overlay.liveY, width: overlay.liveW, height: overlay.liveH}
            : overlay.placement

    readonly property var shownFrame: {
        const rect = overlay.shownRect;
        if (!rect || !(rect.width > 0) || overlay.width <= 0 || overlay.height <= 0 ||
                overlay.canvasWidth <= 0 || overlay.canvasHeight <= 0)
            return null;
        const sx = overlay.width / overlay.canvasWidth;
        const sy = overlay.height / overlay.canvasHeight;
        return {x: rect.x * sx, y: rect.y * sy, w: rect.width * sx, h: rect.height * sy};
    }

    readonly property bool selectionLocked: {
        const held = overlay.dragging ? overlay.hold : overlay.placement;
        return !!(held && held.locked === true);
    }

    function toCanvas(x, y) {
        if (overlay.canvasWidth <= 0 || overlay.canvasHeight <= 0 ||
                overlay.width <= 0 || overlay.height <= 0)
            return Qt.point(0, 0);
        return Qt.point(x * overlay.canvasWidth / overlay.width,
                        y * overlay.canvasHeight / overlay.height);
    }

    function centerOf(edges) {
        const frame = overlay.shownFrame;
        if (!frame)
            return Qt.point(0, 0);
        let x = frame.x;
        let y = frame.y;
        if (edges.indexOf("r") >= 0)
            x = frame.x + frame.w;
        else if (edges.indexOf("l") < 0)
            x = frame.x + frame.w / 2;
        if (edges.indexOf("b") >= 0)
            y = frame.y + frame.h;
        else if (edges.indexOf("t") < 0)
            y = frame.y + frame.h / 2;
        return Qt.point(x, y);
    }

    function cursorFor(edges) {
        if (edges === "lt" || edges === "rb")
            return Qt.SizeFDiagCursor;
        if (edges === "rt" || edges === "lb")
            return Qt.SizeBDiagCursor;
        if (edges === "l" || edges === "r")
            return Qt.SizeHorCursor;
        return Qt.SizeVerCursor;
    }

    function handleAt(x, y) {
        const frame = overlay.shownFrame;
        if (!frame)
            return "";
        for (let i = 0; i < overlay.handleEdges.length; ++i) {
            const edges = overlay.handleEdges[i];
            const center = overlay.centerOf(edges);
            if (Math.abs(x - center.x) <= 8 && Math.abs(y - center.y) <= 8)
                return edges;
        }
        return "";
    }

    function contains(rect, point) {
        return point.x >= rect.x && point.x < rect.x + rect.width &&
               point.y >= rect.y && point.y < rect.y + rect.height;
    }

    function hitNode(x, y) {
        const point = overlay.toCanvas(x, y);
        const nodes = Document.placedNodes(overlay.canvasId);
        for (let i = 0; i < nodes.length; ++i) {
            if (overlay.contains(nodes[i], point))
                return nodes[i];
        }
        if (overlay.placement && overlay.handleAt(x, y) !== "")
            return overlay.placement;
        return null;
    }

    /// The rectangle a handle drag wants, from the press and the keys held now.
    /// Recomputed from the press every move, so letting Shift go returns to the free shape.
    function resized(start, edges, dx, dy, shift, alt) {
        const min = 8;
        const aspect = start.height > 0 ? start.width / start.height : 1;
        const dragX = edges.indexOf("l") >= 0 || edges.indexOf("r") >= 0;
        const dragY = edges.indexOf("t") >= 0 || edges.indexOf("b") >= 0;
        let growX = 0;
        let growY = 0;
        if (edges.indexOf("r") >= 0)
            growX = dx;
        else if (edges.indexOf("l") >= 0)
            growX = -dx;
        if (edges.indexOf("b") >= 0)
            growY = dy;
        else if (edges.indexOf("t") >= 0)
            growY = -dy;
        if (alt) {
            growX *= 2;
            growY *= 2;
        }
        let nw = dragX ? Math.max(min, start.width + growX) : start.width;
        let nh = dragY ? Math.max(min, start.height + growY) : start.height;
        if (shift && aspect > 0) {
            if (dragX && dragY) {
                const rx = Math.abs(nw - start.width) / start.width;
                const ry = Math.abs(nh - start.height) / start.height;
                if (rx >= ry)
                    nh = nw / aspect;
                else
                    nw = nh * aspect;
            } else if (dragX) {
                nh = nw / aspect;
            } else if (dragY) {
                nw = nh * aspect;
            }
            if (nw < min || nh < min) {
                const lift = Math.max(min / nw, min / nh);
                nw *= lift;
                nh *= lift;
            }
        }
        const cx = start.x + start.width / 2;
        const cy = start.y + start.height / 2;
        let nx = start.x;
        let ny = start.y;
        if (alt) {
            nx = (dragX || shift) ? cx - nw / 2 : start.x;
            ny = (dragY || shift) ? cy - nh / 2 : start.y;
        } else {
            if (edges.indexOf("r") >= 0)
                nx = start.x;
            else if (edges.indexOf("l") >= 0)
                nx = start.x + start.width - nw;
            else
                nx = start.x + (start.width - nw) / 2;
            if (edges.indexOf("b") >= 0)
                ny = start.y;
            else if (edges.indexOf("t") >= 0)
                ny = start.y + start.height - nh;
            else
                ny = start.y + (start.height - nh) / 2;
        }
        return {x: nx, y: ny, w: nw, h: nh};
    }

    function press(x, y) {
        canvasMenu.close();
        const selected = overlay.placement;
        const onHandle = selected && selected.locked !== true ? overlay.handleAt(x, y) : "";
        let target = null;
        if (onHandle !== "")
            target = selected;
        else
            target = overlay.hitNode(x, y);
        if (!target)
            return;
        Document.focusPlacedNode(overlay.canvasId, target.id);
        if (target.locked === true)
            return;
        const point = overlay.toCanvas(x, y);
        overlay.hold = target;
        overlay.liveX = target.x;
        overlay.liveY = target.y;
        overlay.liveW = target.width;
        overlay.liveH = target.height;
        overlay.edges = onHandle;
        overlay.originX = point.x;
        overlay.originY = point.y;
        overlay.dragging = true;
        overlay.cursorShape = onHandle === "" ? Qt.SizeAllCursor : overlay.cursorFor(onHandle);
    }

    function drag(x, y, modifiers) {
        if (!overlay.dragging || !overlay.hold)
            return;
        const point = overlay.toCanvas(x, y);
        const dx = point.x - overlay.originX;
        const dy = point.y - overlay.originY;
        const start = overlay.hold;
        if (overlay.edges === "") {
            overlay.liveX = start.x + dx;
            overlay.liveY = start.y + dy;
            overlay.liveW = start.width;
            overlay.liveH = start.height;
            Document.setNodeFrame(start.posX + dx, start.posY + dy, start.scaleX, start.scaleY,
                                  true);
            return;
        }
        const shift = (modifiers & Qt.ShiftModifier) !== 0;
        const alt = (modifiers & Qt.AltModifier) !== 0;
        const box = overlay.resized(start, overlay.edges, dx, dy, shift, alt);
        overlay.liveX = box.x;
        overlay.liveY = box.y;
        overlay.liveW = box.w;
        overlay.liveH = box.h;
        Document.setDrawnRect(box.x, box.y, box.w, box.h, true);
    }

    function release() {
        if (!overlay.dragging)
            return;
        overlay.dragging = false;
        overlay.edges = "";
        overlay.cursorShape = Qt.ArrowCursor;
        Document.endGesture();
    }

    function hover(x, y) {
        if (overlay.placement && overlay.placement.locked !== true) {
            const edges = overlay.handleAt(x, y);
            if (edges !== "") {
                overlay.cursorShape = overlay.cursorFor(edges);
                return;
            }
            if (overlay.contains(overlay.placement, overlay.toCanvas(x, y))) {
                overlay.cursorShape = Qt.SizeAllCursor;
                return;
            }
        }
        overlay.cursorShape = Qt.ArrowCursor;
    }

    function row(text, shortcut, enabled, action) {
        return {text: text, shortcut: shortcut, enabled: enabled, checked: false, action: action};
    }

    function menuRows() {
        const locked = Document.nodeProperties.locked === true;
        const id = Document.selectedNode;
        const sep = {separator: true};
        return [
            {
                text: qsTr("Order"),
                checked: false,
                children: [
                    overlay.row(qsTr("Move Up"), "Ctrl+Up", true,
                                () => Document.moveNode(id, -1)),
                    overlay.row(qsTr("Move Down"), "Ctrl+Down", true,
                                () => Document.moveNode(id, 1)),
                    overlay.row(qsTr("Move to Top"), "Ctrl+Home", true,
                                () => Document.moveSelectedToEnd(true)),
                    overlay.row(qsTr("Move to Bottom"), "Ctrl+End", true,
                                () => Document.moveSelectedToEnd(false))
                ]
            },
            {
                text: qsTr("Transform"),
                checked: false,
                children: [
                    overlay.row(qsTr("Edit Transform"), "Ctrl+E", true,
                                () => Workspace.focusPanel("properties")),
                    overlay.row(qsTr("Copy Transform"), "Ctrl+Shift+C", true,
                                () => Document.copyTransform()),
                    overlay.row(qsTr("Paste Transform"), "Ctrl+Shift+V",
                                !locked && Document.hasCopiedTransform(),
                                () => Document.pasteTransform()),
                    overlay.row(qsTr("Reset Transform"), "Ctrl+R", !locked,
                                () => Document.resetTransform()),
                    sep,
                    overlay.row(qsTr("Rotate 90 degrees CW"), "", !locked,
                                () => Document.rotateBy(90)),
                    overlay.row(qsTr("Rotate 90 degrees CCW"), "", !locked,
                                () => Document.rotateBy(-90)),
                    overlay.row(qsTr("Rotate 180 degrees"), "", !locked,
                                () => Document.rotateBy(180)),
                    sep,
                    overlay.row(qsTr("Flip Horizontal"), "", !locked,
                                () => Document.flipHorizontal()),
                    overlay.row(qsTr("Flip Vertical"), "", !locked,
                                () => Document.flipVertical()),
                    sep,
                    overlay.row(qsTr("Fit to screen"), "Ctrl+F", !locked,
                                () => Document.fitToCanvas(false)),
                    // Ctrl+S is Save. The item is here; it does not wear that key.
                    overlay.row(qsTr("Stretch to screen"), "", !locked,
                                () => Document.fitToCanvas(true)),
                    overlay.row(qsTr("Center to screen"), "Ctrl+D", !locked,
                                () => Document.centerOnCanvas(true, true)),
                    overlay.row(qsTr("Center Vertically"), "", !locked,
                                () => Document.centerOnCanvas(false, true)),
                    overlay.row(qsTr("Center Horizontally"), "", !locked,
                                () => Document.centerOnCanvas(true, false))
                ]
            },
            sep,
            overlay.row(qsTr("Copy"), "Ctrl+C", true, () => Document.copySelectedNodes()),
            overlay.row(qsTr("Paste"), "Ctrl+V", Document.pasteDescription() !== "",
                        () => Document.paste()),
            sep,
            overlay.row(qsTr("Remove"), "Del", true, () => Document.removeSelectedNodes()),
            overlay.row(qsTr("Rename..."), "F2", Document.selectedNodes.length === 1,
                        () => Document.requestRename())
        ];
    }

    function openMenu(x, y) {
        const target = overlay.hitNode(x, y);
        if (!target)
            return;
        // A right-click on something already selected keeps the rest of the selection, so the
        // menu can act on all of it. The same rule as the sources list.
        if (Document.selectedNodes.indexOf(target.id) < 0)
            Document.focusPlacedNode(overlay.canvasId, target.id);
        canvasMenu.popup(x, y, overlay.menuRows());
    }

    Rectangle {
        objectName: "programSelection"
        visible: overlay.shownFrame !== null
        x: overlay.shownFrame ? overlay.shownFrame.x : 0
        y: overlay.shownFrame ? overlay.shownFrame.y : 0
        width: overlay.shownFrame ? overlay.shownFrame.w : 0
        height: overlay.shownFrame ? overlay.shownFrame.h : 0
        color: "transparent"
        border.width: 1
        border.color: overlay.selectionLocked ? Theme.c.textMuted : Theme.c.accent
    }

    Repeater {
        model: overlay.handleEdges

        delegate: Rectangle {
            required property string modelData
            objectName: "programHandle"
            visible: overlay.shownFrame !== null && !overlay.selectionLocked
            width: 8
            height: 8
            x: overlay.centerOf(modelData).x - 4
            y: overlay.centerOf(modelData).y - 4
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.accent
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        preventStealing: true
        cursorShape: overlay.cursorShape
        enabled: overlay.canvasWidth > 0 && overlay.canvasHeight > 0

        onPressed: (ev) => {
            if (ev.button === Qt.RightButton)
                overlay.openMenu(ev.x, ev.y);
            else
                overlay.press(ev.x, ev.y);
        }
        onPositionChanged: (ev) => {
            if (overlay.dragging)
                overlay.drag(ev.x, ev.y, ev.modifiers);
            else
                overlay.hover(ev.x, ev.y);
        }
        onReleased: overlay.release()
        onCanceled: overlay.release()
    }

    StripMenu {
        id: canvasMenu
        objectName: "canvasMenu"
    }
}
