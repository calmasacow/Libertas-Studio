// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The bus graph: one strip per bus, and the selected source's own routing beside it.
///
/// There are no meters, and the panel says so rather than drawing some. Nothing in this build
/// makes a sound -- there is no audio host yet -- so a moving meter here would be an animation on
/// a timer, and the panel would look finished while being incapable. What *is* real is everything
/// the document holds: the buses, their kinds, faders, pans, mutes and solos, which bus each
/// source node feeds, and the count of them feeding each bus. All of it is commands, so all of it
/// undoes.
///
/// A dragged fader runs a command per move and the session collapses the run into one undo
/// step, landing back where the drag began. That is what `Document.setBusGain(..., dragging)`
/// and `endGesture()` are for; without the collapsing, one drag would bury every other edit
/// under four hundred "Change bus level" entries.
Item {
    id: panel

    property bool adding: false
    /// Below this the routing line goes, because a strip's fader matters more than a sentence
    /// about the selected source -- the arrow on each strip still routes it. 130 is the panel's
    /// minimum height in llayout, and this has to leave a usable strip at that size.
    readonly property bool showRouting: panel.height >= 150
    /// dB at the top and bottom of a fader. -60 reads as off without being a special case.
    readonly property real gainMin: -60
    readonly property real gainMax: 12

    function faderFraction(db) {
        return (Math.max(panel.gainMin, Math.min(panel.gainMax, db)) - panel.gainMin) /
               (panel.gainMax - panel.gainMin);
    }

    function faderDb(fraction) {
        const db = panel.gainMin + Math.max(0, Math.min(1, fraction)) *
                   (panel.gainMax - panel.gainMin);
        // Quarter-dB steps: fine enough to be a fader, coarse enough that nudging it twice to
        // the same place does not land two commands on the undo stack.
        return Math.round(db * 4) / 4;
    }

    function dbLabel(db) {
        if (db <= panel.gainMin)
            return "-∞";
        return (db > 0 ? "+" : "") + db.toFixed(1);
    }

    ListView {
        id: strips

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: routing.top
        orientation: ListView.Horizontal
        clip: true
        model: Document.buses
        boundsBehavior: Flickable.StopAtBounds

        delegate: Item {
            id: strip

            required property string busId
            required property string name
            required property string kind
            required property real gainDb
            required property real pan
            required property bool mute
            required property bool solo
            required property int channels
            required property int feedCount
            required property int sendCount
            required property bool carriesSelection

            objectName: "busStrip"
            width: 84
            height: ListView.view.height

            /// What the fader is showing right now. It follows the document except while being
            /// dragged, so the value under the finger is not fighting the model's.
            property real shownGain: strip.gainDb
            property bool dragging: false

            /// A docked panel can be made short, and a mixer has to survive it. Below this the
            /// strip drops the kind line, the feed count and the pan -- in that order of value
            /// to someone running a show -- rather than squeezing the fader into nothing. The
            /// panel's own minimum height (llayout's studio_defaults) is the point at which
            /// what is left is still usable.
            readonly property bool compact: strip.height < 150
            /// Everything in the column that is not the fader, so the fader can have the rest.
            readonly property real fixedHeight:
                16 + 14 + 20 + (strip.compact ? 0 : 12 + 12 + 10) +
                3 * ((strip.compact ? 4 : 7) - 1)

            onGainDbChanged: {
                if (!strip.dragging)
                    strip.shownGain = strip.gainDb;
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                radius: 2
                color: strip.carriesSelection ? Theme.c.panelHeaderActive : Theme.c.panelHeader
                border.width: 1
                border.color: strip.carriesSelection ? Theme.c.accent : Theme.c.border
            }

            Column {
                id: column

                anchors.fill: parent
                anchors.margins: 6
                spacing: 3

                Item {
                    width: parent.width
                    height: 16

                    Text {
                        anchors.fill: parent
                        visible: !rename.visible
                        text: strip.name
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Theme.c.text
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        MouseArea {
                            anchors.fill: parent
                            onDoubleClicked: {
                                rename.text = strip.name;
                                rename.visible = true;
                                rename.forceActiveFocus();
                                rename.selectAll();
                            }
                        }
                    }

                    TextInput {
                        id: rename

                        anchors.fill: parent
                        visible: false
                        font.pixelSize: 11
                        color: Theme.c.text
                        selectionColor: Theme.c.accent
                        selectByMouse: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter

                        onAccepted: {
                            visible = false;
                            if (text !== strip.name)
                                Document.renameBus(strip.busId, text);
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus)
                                visible = false;
                        }
                        Keys.onEscapePressed: visible = false
                    }
                }

                Text {
                    width: parent.width
                    height: 12
                    visible: !strip.compact
                    text: strip.kind + " · " + strip.channels + "ch"
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    opacity: 0.8
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                // What feeds this bus. Zero is worth saying out loud: a bus nothing routes to
                // looks exactly like a working one until the show starts.
                Text {
                    width: parent.width
                    height: 12
                    visible: !strip.compact
                    text: strip.feedCount === 0
                        ? qsTr("nothing feeding")
                        : qsTr("%n source(s)", "", strip.feedCount)
                    font.pixelSize: 9
                    color: strip.feedCount === 0 ? Theme.c.warning : Theme.c.textMuted
                    opacity: strip.feedCount === 0 ? 0.9 : 0.7
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }

                // The fader takes whatever the rows above and below did not, which is why they
                // all declare a height: a Column cannot be asked for "the rest".
                Item {
                    id: faderArea

                    width: parent.width
                    height: Math.max(column.height - strip.fixedHeight, 24)

                    Rectangle {
                        id: track

                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 6
                        radius: 3
                        color: Theme.c.window
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    // Unity, so the fader has somewhere to be read against.
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: (faderArea.height - knob.height) * (1 - panel.faderFraction(0)) +
                           knob.height / 2
                        width: 16
                        height: 1
                        color: Theme.c.border
                    }

                    // Filled from the bottom up to where the fader sits.
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.bottom: parent.bottom
                        width: 6
                        radius: 3
                        height: faderArea.height * panel.faderFraction(strip.shownGain)
                        color: strip.mute ? Theme.c.textMuted : Theme.c.accent
                        opacity: strip.mute ? 0.4 : 0.75
                    }

                    Rectangle {
                        id: knob

                        anchors.horizontalCenter: parent.horizontalCenter
                        y: (faderArea.height - height) *
                           (1 - panel.faderFraction(strip.shownGain))
                        width: 24
                        height: 10
                        radius: 2
                        color: faderMouse.pressed ? Theme.c.accent : Theme.c.panelHeaderActive
                        border.width: 1
                        border.color: faderMouse.containsMouse || faderMouse.pressed
                            ? Theme.c.accent : Theme.c.border
                    }

                    MouseArea {
                        id: faderMouse

                        objectName: "busFader"
                        anchors.fill: parent
                        hoverEnabled: true

                        function dbAt(y) {
                            const usable = faderArea.height - knob.height;
                            if (usable <= 0)
                                return strip.shownGain;
                            return panel.faderDb(1 - (y - knob.height / 2) / usable);
                        }

                        onPressed: (mouse) => {
                            strip.dragging = true;
                            strip.shownGain = dbAt(mouse.y);
                            Document.setBusGain(strip.busId, strip.shownGain, true);
                        }
                        onPositionChanged: (mouse) => {
                            if (!strip.dragging)
                                return;
                            strip.shownGain = dbAt(mouse.y);
                            Document.setBusGain(strip.busId, strip.shownGain, true);
                        }
                        onReleased: {
                            strip.dragging = false;
                            Document.setBusGain(strip.busId, strip.shownGain, true);
                            // Letting go is what separates one drag from the next.
                            Document.endGesture();
                        }
                        onCanceled: {
                            // Grab stolen mid-drag: close the gesture and go back to following
                            // the document, which already holds wherever the drag got to.
                            strip.dragging = false;
                            Document.endGesture();
                            strip.shownGain = strip.gainDb;
                        }
                        // A wheel is the keyboard-adjacent way to trim a fader, and one notch is
                        // one command, so it does not need the drag treatment.
                        onWheel: (wheel) => {
                            const step = wheel.modifiers & Qt.ShiftModifier ? 0.25 : 1.0;
                            Document.setBusGain(
                                strip.busId,
                                Math.max(panel.gainMin,
                                         Math.min(panel.gainMax,
                                                  strip.gainDb +
                                                  (wheel.angleDelta.y > 0 ? step : -step))));
                        }
                        onDoubleClicked: Document.setBusGain(strip.busId, 0)
                    }
                }

                Text {
                    width: parent.width
                    height: 14
                    text: panel.dbLabel(strip.shownGain) + " dB"
                    font.pixelSize: 10
                    font.family: "monospace"
                    color: strip.dragging ? Theme.c.accent : Theme.c.text
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                Item {
                    width: parent.width
                    height: 20

                    Row {
                        anchors.centerIn: parent
                        spacing: 3

                        ToolButton {
                            label: "M"
                            tip: strip.mute ? qsTr("Unmute this bus") : qsTr("Mute this bus")
                            active: strip.mute
                            activeColor: Theme.c.error
                            onClicked: Document.setBusMute(strip.busId, !strip.mute)
                        }

                        ToolButton {
                            label: "S"
                            tip: strip.solo ? qsTr("Clear solo") : qsTr("Solo this bus")
                            active: strip.solo
                            activeColor: Theme.c.warning
                            onClicked: Document.setBusSolo(strip.busId, !strip.solo)
                        }

                        ToolButton {
                            label: "→"
                            tip: qsTr("Send the selected source's audio here")
                            enabled: Document.selectedNode !== "" &&
                                     Document.nodeProperties.hasAudio === true
                            onClicked: Document.routeSelectedNode(strip.busId)
                        }
                    }
                }

                // Pan, as a strip rather than a knob: it is the one control here that reads
                // better as a position than as a number.
                Item {
                    width: parent.width
                    height: 10
                    visible: !strip.compact

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: 3
                        radius: 1
                        color: Theme.c.window
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        x: (parent.width - width) * (strip.pan + 1) / 2
                        width: 8
                        height: 8
                        radius: 4
                        color: panMouse.containsMouse
                            ? Theme.c.accent : Theme.c.panelHeaderActive
                        border.width: 1
                        border.color: Theme.c.border
                    }

                    MouseArea {
                        id: panMouse

                        objectName: "busPan"
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        onClicked: (mouse) => {
                            Document.setBusPan(strip.busId,
                                               Math.round((mouse.x / width * 2 - 1) * 20) / 20);
                        }
                        onDoubleClicked: Document.setBusPan(strip.busId, 0)
                    }
                }
            }
        }
    }

    // Where the selected source's audio goes. Routing lives here rather than in the properties
    // panel because the question "which bus is this on" is answered by looking at the buses.
    Rectangle {
        id: routing

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: kindPicker.visible ? kindPicker.top : footer.top
        height: visible ? 30 : 0
        visible: panel.showRouting
        color: Theme.c.panelBackground

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.c.border
        }

        Text {
            id: routingText

            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: routingActions.left
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            font.pixelSize: 10
            color: Theme.c.textMuted
            elide: Text.ElideRight
            text: {
                if (Document.selectedNode === "")
                    return qsTr("Select a source to route its audio.");
                const props = Document.nodeProperties;
                if (props.hasAudio !== true)
                    return qsTr("%1 has no audio of its own.").arg(props.name);
                if (props.audioBus === "")
                    // Unrouted is not muted, and the difference matters: one is silent on
                    // purpose and the other is silent by omission.
                    return qsTr("%1 is not routed anywhere — it is silent.").arg(props.name);
                return qsTr("%1 → %2 at %3 dB%4")
                    .arg(props.name)
                    .arg(props.audioBusName)
                    .arg(panel.dbLabel(props.audioGain))
                    .arg(props.audioMute ? qsTr(" (muted)") : "");
            }
        }

        Row {
            id: routingActions

            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            ToolButton {
                label: "M"
                tip: qsTr("Mute the selected source")
                enabled: Document.nodeProperties.hasAudio === true
                active: Document.nodeProperties.audioMute === true
                activeColor: Theme.c.error
                onClicked: Document.setSelectedNodeMute(
                    Document.nodeProperties.audioMute !== true)
            }

            ToolButton {
                label: "⊘"
                tip: qsTr("Unroute the selected source")
                enabled: Document.nodeProperties.hasAudio === true &&
                         Document.nodeProperties.audioBus !== ""
                onClicked: Document.routeSelectedNode("")
            }
        }
    }

    Rectangle {
        id: kindPicker

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        height: visible ? Math.min(kindList.contentHeight + 22, 180) : 0
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
            text: qsTr("New bus")
            font.pixelSize: 10
            font.weight: Font.DemiBold
            color: Theme.c.accent
        }

        ListView {
            id: kindList

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: pickerCaption.bottom
            anchors.topMargin: 2
            anchors.bottom: parent.bottom
            clip: true
            model: Document.busKinds
            boundsBehavior: Flickable.StopAtBounds

            delegate: Rectangle {
                id: kindRow

                required property var modelData

                width: kindList.width
                height: 20
                color: kindMouse.containsMouse ? Theme.c.accent : "transparent"

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: kindRow.modelData.title
                    font.pixelSize: 11
                    color: kindMouse.containsMouse ? Theme.c.window : Theme.c.text
                }

                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: kindRow.modelData.note
                    font.pixelSize: 9
                    color: kindMouse.containsMouse ? Theme.c.window : Theme.c.textMuted
                    opacity: 0.8
                }

                MouseArea {
                    id: kindMouse

                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: {
                        Document.addBus(kindRow.modelData.title, kindRow.modelData.id);
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
                tip: panel.adding ? qsTr("Cancel") : qsTr("Add bus")
                onClicked: panel.adding = !panel.adding
            }
        }

        // Says what this panel cannot do, where it cannot be mistaken for something it can.
        Text {
            anchors.centerIn: parent
            text: qsTr("no audio host — levels only")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
        }
    }
}
