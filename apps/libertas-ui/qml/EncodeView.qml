// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The Encode workspace: where every bitstream and every destination in the show is set up.
///
/// Deliberately not a dock arrangement. The other two workspaces are panels an operator
/// rearranges; this is one picture of one graph, and a version of it with the properties
/// column dragged somewhere else would be a worse picture of the same graph. ADR-0015 keeps it
/// on the same single surface either way -- this is a view inside that surface, not a window.
///
/// Three columns, left to right: what is being encoded, what encodes it, where it goes. That
/// is also the direction every edge runs, which is what makes the picture readable at a glance.
Item {
    id: view

    objectName: "encodeView"

    /// Which node the properties column is showing. View state, not document state: nothing
    /// about where someone is looking belongs in a file that goes to another machine.
    property string selectedKind: ""
    property string selectedId: ""
    property string initializedProjectPath: ""
    function initializeProject() {
        initializedProjectPath = Document.projectPath;
        Document.initializeEncodeGraph();
    }
    Component.onCompleted: initializeProject()

    /// Record follows the Recording output's route, regardless of the current selection.
    property int revision: 0
    readonly property string recordTarget: view.revision >= 0 ? Document.recordingEncodeId() : ""

    Connections {
        target: Document
        function onChanged() {
            view.revision++;
            if (view.initializedProjectPath !== Document.projectPath)
                Qt.callLater(view.initializeProject);
        }
    }

    function bytesLabel(n) {
        if (n >= 1024 * 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024 * 1024)).toFixed(1) + " TB";
        if (n >= 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB";
        return (n / (1024 * 1024)).toFixed(1) + " MB";
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
    }

    // --- the top bar -------------------------------------------------------------------------

    Item {
        id: topBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 46

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Encoder Flow")
            font.pixelSize: 19
            font.weight: Font.DemiBold
            color: Theme.c.accent
        }

        Row {
            anchors.right: actions.left
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 22

            Column {
                spacing: 1

                Text {
                    text: Telemetry.diskAvailable
                          ? view.bytesLabel(Telemetry.diskFreeBytes)
                          : "—"
                    font.pixelSize: 12
                    color: Theme.c.text
                }

                Text {
                    text: qsTr("Disk space")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }
            }

            Column {
                spacing: 1

                Text {
                    // The first canvas's rate. Not a measured figure -- nothing is encoding
                    // yet -- so it says the target rather than inventing a number.
                    text: canvasFps.value > 0 ? canvasFps.value.toFixed(1) : "—"
                    font.pixelSize: 12
                    color: Theme.c.text
                }

                Text {
                    text: qsTr("FPS")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                }
            }
        }

        QtObject {
            id: canvasFps

            // An empty id means the first canvas. A call, so the Connections below is what
            // makes it re-read; Document emits `changed` after every command.
            property real value: 0

            function refresh() { canvasFps.value = Document.canvasFpsOf(""); }
            Component.onCompleted: refresh()
        }

        Connections {
            target: Document
            function onChanged() { canvasFps.refresh(); }
        }

        Row {
            id: actions

            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // Nothing behind these yet. Drawn because the mockup places them and an operator
            // should see where they will be, disabled because a button that looks live and
            // does nothing is worse than one that says it cannot.
            ToolButton {
                width: 96
                height: 28
                label: qsTr("Go Live!")
                enabled: false
                tip: qsTr("Streaming is not connected yet")
            }

            // The Recording node names the bitstreams this action writes.
            ToolButton {
                objectName: "encodeRecord"
                width: 96
                height: 28
                label: Preview.recording ? qsTr("Stop") : qsTr("Record")
                active: Preview.recording
                activeColor: Theme.c.error
                enabled: Preview.recording || view.recordTarget !== ""
                tip: Preview.recording
                     ? qsTr("Stop recording and close the file")
                     : view.recordTarget !== ""
                       ? qsTr("Record the Recording output to a file beside the project")
                       : qsTr("Connect a Recording destination to video and audio encoders")
                onClicked: {
                    if (Preview.recording) {
                        Preview.stopRecording(view.recordTarget);
                        return;
                    }
                    const path = Document.recordingPathFor(view.recordTarget);
                    if (path !== "")
                        Preview.startRecording(view.recordTarget, path);
                }
            }

            ToolButton {
                width: 96
                height: 28
                label: qsTr("Replay")
                enabled: false
                tip: qsTr("The replay buffer is not built yet")
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.c.border
        }
    }

    // --- the graph ---------------------------------------------------------------------------

    EncodeGraph {
        id: graph

        anchors.left: parent.left
        anchors.right: properties.left
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom

        selectedKind: view.selectedKind
        selectedId: view.selectedId
        onNodeSelected: (kind, id) => {
            view.selectedKind = kind;
            view.selectedId = id;
        }
        onRemoveRequested: (kind, id, name) => {
            removal.kind = kind;
            removal.nodeId = id;
            removal.nodeName = name;
            removal.forceActiveFocus();
        }
    }

    // The two buttons that add to the graph, over it at the bottom left so they stay put while
    // it is panned -- they act on the project, not on the part of it currently on screen.
    Column {
        anchors.left: parent.left
        anchors.leftMargin: 26
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 22
        spacing: 10

        ToolButton {
            objectName: "addEncoder"
            width: 220
            height: 42
            label: qsTr("+ New Encoder")
            tip: qsTr("Add an encode of the current canvas, with one destination")
            onClicked: Document.addEncode()
        }

        ToolButton {
            objectName: "addAudioEncoder"
            width: 220
            height: 42
            label: qsTr("+ New Audio Encoder")
            tip: qsTr("Add an audio encode of the program bus")
            onClicked: Document.addAudioEncode()
        }

        ToolButton {
            objectName: "addDestination"
            width: 220
            height: 42
            label: qsTr("+ New Destination")
            // An encode first, because a destination with no bitstream is not a state the
            // model has a meaning for (ADR-0029).
            enabled: Document.encodeCount() > 0
            tip: Document.encodeCount() > 0
                 ? qsTr("Add a destination for the first encode")
                 : qsTr("Add an encoder first; a destination needs a bitstream to send")
            onClicked: {
                const encodes = Document.encodeChoices();
                if (encodes.length > 0)
                    Document.addOutputOnEncode(encodes[0].id, qsTr("Destination"));
            }
        }
    }

    // --- the properties column ---------------------------------------------------------------

    EncodeProperties {
        id: properties

        objectName: "encodeProperties"
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom
        // The mockup's proportion. Narrow enough that the graph keeps the room, wide enough
        // that "Two Passes (Quarter Resolution)" fits in a dropdown without eliding.
        width: Math.min(520, Math.max(440, view.width * 0.20))

        selectedKind: view.selectedKind
        selectedId: view.selectedId
    }

    Item {
        id: removal
        objectName: "encodeRemovalConfirmation"
        anchors.fill: parent
        z: 20
        property string kind: ""
        property string nodeId: ""
        property string nodeName: ""
        visible: nodeId !== ""
        focus: visible

        function dismiss() { nodeId = ""; }
        function confirm() {
            if (kind === "encode")
                Document.removeEncode(nodeId);
            else if (kind === "audio-encode")
                Document.removeAudioEncode(nodeId);
            else if (kind === "output")
                Document.removeOutput(nodeId);
            view.selectedKind = "";
            view.selectedId = "";
            dismiss();
        }
        Keys.onEscapePressed: dismiss()
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: removal.dismiss()
            onWheel: wheel => { wheel.accepted = true; }
        }
        Rectangle {
            anchors.fill: parent
            color: Theme.c.window
            opacity: 0.55
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 40, 430)
            height: message.height + buttons.height + 42
            color: Theme.c.panelBackground
            border.width: 1
            border.color: Theme.c.warning
            radius: 3
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
            }
            Column {
                id: message
                x: 14
                y: 14
                width: parent.width - 28
                spacing: 8
                Text {
                    width: parent.width
                    text: qsTr("Remove “%1”?").arg(removal.nodeName)
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    font.bold: true
                    color: Theme.c.text
                }
                Text {
                    width: parent.width
                    text: removal.kind === "encode"
                        ? qsTr("This also removes every destination connected to this video encoder. You can undo this change.")
                        : removal.kind === "audio-encode"
                        ? qsTr("Connected destinations will lose this audio track. You can undo this change.")
                        : qsTr("The connected encoders will be kept. You can undo this change.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 14
                spacing: 8
                DialogButton {
                    objectName: "cancelEncodeRemoval"
                    text: qsTr("Cancel")
                    onClicked: removal.dismiss()
                }
                DialogButton {
                    objectName: "confirmEncodeRemoval"
                    text: qsTr("Remove")
                    primary: true
                    onClicked: removal.confirm()
                }
            }
        }
    }
}
