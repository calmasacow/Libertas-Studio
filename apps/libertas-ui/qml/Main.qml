// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

// ADR-0015 rule 1: one top-level window holds the entire workspace. Panels are regions inside
// this surface, laid out by our own engine. There is no second OS window, which is why nothing
// here can be moved off-screen, lost behind another application, or closed by accident.
Window {
    id: root

    visible: true
    width: 1600
    height: 950
    minimumWidth: 960
    minimumHeight: 620
    title: Document.projectName + (Document.dirty ? " \u2022 " : " \u2014 ") +
           qsTr("Libertas Studio")
    color: Theme.c.window

    // A line of text the status bar shows for a few seconds: a refused drop, a failed save, a
    // theme missing tokens. Transient, never a dialog -- nothing here should ever interrupt a
    // show to be acknowledged.
    property string notice: ""

    Timer {
        id: noticeTimer
        interval: 7000
        onTriggered: root.notice = ""
    }

    Connections {
        target: Workspace
        function onNotice(text) {
            root.notice = text;
            noticeTimer.restart();
        }
    }

    Connections {
        target: Theme
        function onNotice(text) {
            root.notice = text;
            noticeTimer.restart();
        }
    }

    Connections {
        target: Document
        function onNotice(text) {
            root.notice = text;
            noticeTimer.restart();
        }
    }

    // ADR-0016 rule 7: Escape cancels an in-flight drag. A mis-drop costs one keystroke.
    Shortcut {
        sequences: ["Escape"]
        onActivated: Workspace.cancelDrag()
    }
    // Ctrl+Z is the *document's* undo, because that is what it is everywhere else and an
    // operator should never have to work out which stack it will reach. ADR-0021 kept the two
    // stacks separate precisely so this choice was available; the layout stack is still there,
    // on its own binding, which is also how Premiere and After Effects treat a workspace change.
    Shortcut {
        sequences: ["Ctrl+Z"]
        onActivated: Document.undo()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        onActivated: Document.redo()
    }
    Shortcut {
        sequences: ["Ctrl+S"]
        onActivated: Document.save()
    }
    Shortcut {
        sequences: ["Ctrl+Alt+Z"]
        onActivated: Workspace.undo()
    }
    Shortcut {
        sequences: ["Ctrl+Alt+Shift+Z"]
        onActivated: Workspace.redo()
    }
    Shortcut {
        sequences: ["Ctrl+Shift+R"]
        onActivated: Workspace.resetLayout()
    }

    Item {
        id: workArea

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: statusBar.top

        onWidthChanged: Workspace.setViewport(width, height)
        onHeightChanged: Workspace.setViewport(width, height)
        Component.onCompleted: Workspace.setViewport(width, height)

        // One mouse area for every divider, beneath the panels.
        //
        // Dividers are hit-tested by the layout engine rather than by per-divider MouseAreas,
        // because a resize changes the layout on every mouse move, which rebuilds the divider
        // delegates -- destroying the very item holding the pointer grab. Keeping the grab on an
        // item that outlives the layout is not an optimisation, it is the only way the gesture
        // can work at all. Panels sit above this and consume their own events, so it only ever
        // sees the gaps.
        MouseArea {
            id: resizeArea

            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton

            property int hoverIndex: -1
            property int hoverAxis: -1
            property int activeAxis: -1
            property bool resizing: false

            cursorShape: {
                const axis = resizing ? activeAxis : hoverAxis;
                if (axis === 0)
                    return Qt.SizeHorCursor;
                if (axis === 1)
                    return Qt.SizeVerCursor;
                return Qt.ArrowCursor;
            }

            onExited: {
                hoverIndex = -1;
                hoverAxis = -1;
            }

            onPositionChanged: mouse => {
                if (resizing) {
                    Workspace.dragDividerTo(mouse.x, mouse.y);
                    return;
                }
                hoverAxis = Workspace.dividerAxisAtPoint(mouse.x, mouse.y);
                hoverIndex = Workspace.dividerIndexAtPoint(mouse.x, mouse.y);
            }

            onPressed: mouse => {
                activeAxis = Workspace.dividerAxisAtPoint(mouse.x, mouse.y);
                resizing = Workspace.beginDividerDrag(mouse.x, mouse.y);
            }

            onReleased: {
                if (resizing) {
                    resizing = false;
                    Workspace.endDividerDrag();
                }
            }

            onCanceled: {
                if (resizing) {
                    resizing = false;
                    Workspace.endDividerDrag();
                }
            }
        }

        // The dividers themselves, drawn but not interactive.
        Repeater {
            model: Workspace.dividers

            delegate: Rectangle {
                id: dividerVisual

                required property int index
                required property bool horizontal
                required property real rectX
                required property real rectY
                required property real rectW
                required property real rectH

                readonly property bool hovered: resizeArea.hoverIndex === index

                x: rectX
                y: rectY
                width: rectW
                height: rectH
                color: hovered ? Theme.c.dividerHover : Theme.c.divider

                Behavior on color {
                    ColorAnimation {
                        duration: 90
                    }
                }

                // Three dots, the universal "this is draggable".
                Row {
                    visible: dividerVisual.horizontal
                    anchors.centerIn: parent
                    spacing: 0

                    Repeater {
                        model: 3

                        delegate: Item {
                            width: 2
                            height: 10

                            Rectangle {
                                anchors.centerIn: parent
                                width: 2
                                height: 2
                                radius: 1
                                color: Theme.c.textMuted
                                opacity: dividerVisual.hovered ? 0.0 : 0.7
                            }
                        }
                    }
                }

                Column {
                    visible: !dividerVisual.horizontal
                    anchors.centerIn: parent
                    spacing: 0

                    Repeater {
                        model: 3

                        delegate: Item {
                            width: 10
                            height: 2

                            Rectangle {
                                anchors.centerIn: parent
                                width: 2
                                height: 2
                                radius: 1
                                color: Theme.c.textMuted
                                opacity: dividerVisual.hovered ? 0.0 : 0.7
                            }
                        }
                    }
                }
            }
        }

        // The panels. The model updates rows in place when the count has not changed, so a
        // divider drag re-evaluates geometry without tearing down and rebuilding every delegate
        // sixty times a second.
        Repeater {
            model: Workspace.panels

            delegate: PanelFrame {
                surface: workArea
            }

        }

        DropOverlay {
            anchors.fill: parent
        }

        DragGhost {}
    }

    StatusBar {
        id: statusBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        notice: root.notice
    }
}
