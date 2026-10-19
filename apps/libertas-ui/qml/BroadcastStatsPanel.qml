// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// How the broadcast is actually going.
///
/// Two kinds of number live here and they are drawn differently on purpose.
///
/// The machine's figures — CPU, memory, free disk, GPU load, encoder load, VRAM — are measured
/// right now and are real. The show's figures — frame rate, render time, frames missed to
/// rendering lag belong to preview compositing and are reported per canvas. Encoding and output
/// figures remain unavailable until an encoder exists.
///
/// Not as zero. "0 / 0 (0.0%)" dropped frames reads as *nothing is being dropped*, which is a
/// claim about a running show; "—" says there is no show. Those are different sentences and only
/// one of them is true.
///
/// The encoder figure is separate from GPU load because it is the one that catches people out: a
/// GPU sitting at 40% whose video encode engine is pinned at 100% drops frames while looking
/// idle, and on this machine the second stream is what saturates it.
Item {
    id: panel

    function bytesLabel(n) {
        if (n >= 1024 * 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024 * 1024)).toFixed(1) + " TB";
        if (n >= 1024 * 1024 * 1024)
            return (n / (1024 * 1024 * 1024)).toFixed(1) + " GB";
        return (n / (1024 * 1024)).toFixed(1) + " MB";
    }

    readonly property string unavailable: "—"
    /// Below this the five-column table does not fit, so each output becomes two lines instead.
    /// The panel folds rather than demanding width, because its minimum size constrains every
    /// panel docked near it, not just itself.
    readonly property bool wideEnough: panel.width >= 340

    Flickable {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: footer.top
        contentHeight: body.implicitHeight + 20
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Nothing here scrolls by being dragged (ADR-0039). The wheel has to come back,
        // because Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: body

            x: 10
            y: 8
            width: parent.width - 20
            spacing: 2

            Text {
                width: parent.width
                text: qsTr("This machine")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                bottomPadding: 4
            }

            StatRow {
                width: parent.width
                label: qsTr("CPU usage")
                value: Telemetry.cpuAvailable
                    ? Telemetry.cpuPercent.toFixed(1) + " %" : panel.unavailable
            }

            StatRow {
                width: parent.width
                label: qsTr("Memory usage")
                value: Telemetry.memoryAvailable
                    ? panel.bytesLabel(Telemetry.memoryBytes) : panel.unavailable
            }

            StatRow {
                width: parent.width
                label: qsTr("Disk space available")
                value: Telemetry.diskAvailable
                    ? panel.bytesLabel(Telemetry.diskFreeBytes) : panel.unavailable
            }

            StatRow {
                width: parent.width
                label: qsTr("Disk full in (approx.)")
                // Needs a recording bitrate to divide by, and there is no recording.
                value: panel.unavailable
            }

            Text {
                width: parent.width
                text: Telemetry.gpuAvailable
                    ? qsTr("Graphics — %1").arg(Telemetry.gpuName)
                    : qsTr("Graphics")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
                elide: Text.ElideRight
            }

            StatRow {
                width: parent.width
                label: qsTr("GPU usage")
                value: Telemetry.gpuAvailable
                    ? Telemetry.gpuPercent.toFixed(0) + " %" : panel.unavailable
            }

            StatRow {
                width: parent.width
                label: qsTr("Encoder usage")
                value: Telemetry.encoderAvailable
                    ? Telemetry.encoderPercent.toFixed(0) + " %" : panel.unavailable
            }

            StatRow {
                width: parent.width
                label: qsTr("VRAM usage")
                value: Telemetry.gpuAvailable && Telemetry.vramTotalBytes > 0
                    ? panel.bytesLabel(Telemetry.vramUsedBytes) + " / " +
                      panel.bytesLabel(Telemetry.vramTotalBytes) +
                      "  (" + (Telemetry.vramUsedBytes / Telemetry.vramTotalBytes * 100).toFixed(0) + " %)"
                    : panel.unavailable
            }

            Text {
                width: parent.width
                visible: !Telemetry.gpuAvailable && Telemetry.gpuNote !== ""
                text: Telemetry.gpuNote
                wrapMode: Text.WordWrap
                font.pixelSize: 9
                color: Theme.c.textMuted
                bottomPadding: 2
            }

            Text {
                width: parent.width
                text: qsTr("The show")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            Repeater {
                model: Document.canvases
                delegate: Column {
                    required property string canvasId
                    required property string name
                    readonly property var stats: {
                        Preview.generation
                        return Preview.renderStats(canvasId)
                    }
                    width: body.width
                    spacing: 1
                    Text {
                        width: parent.width
                        text: name
                        font.pixelSize: 9
                        font.weight: Font.DemiBold
                        color: Theme.c.textMuted
                        topPadding: 5
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Canvas target FPS")
                        value: stats.available && stats.fps > 0
                            ? stats.fps.toFixed(1) : panel.unavailable
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Average time to render frame")
                        value: stats.available && stats.frames > 0
                            ? stats.renderMs.toFixed(2) + " ms" : panel.unavailable
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Frames rendered since reset")
                        value: stats.available ? stats.frames : panel.unavailable
                    }
                    StatRow {
                        width: parent.width
                        label: qsTr("Frames missed due to rendering lag")
                        value: stats.available ? stats.missed : panel.unavailable
                    }
                }
            }

            StatRow {
                width: parent.width
                label: qsTr("Skipped frames due to encoding lag")
                value: panel.unavailable
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: Preview.rendererAttached
                    ? qsTr("Render time and missed frames describe preview compositing. Encoding stats appear when an output encoder is running.")
                    : qsTr("Waiting for renderer statistics.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 4
                bottomPadding: 6
            }

            // ---- the per-output table ----
            Rectangle {
                width: parent.width
                height: 1
                color: Theme.c.border
            }

            Item {
                width: parent.width
                height: 18
                visible: panel.wideEnough

                Text {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.30
                    text: qsTr("Output")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.30
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.18
                    text: qsTr("Status")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.48
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.22
                    text: qsTr("Dropped (net)")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.70
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.16
                    text: qsTr("Sent")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: parent.width * 0.86
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width * 0.14
                    text: qsTr("Bitrate")
                    font.pixelSize: 9
                    font.weight: Font.DemiBold
                    color: Theme.c.textMuted
                    elide: Text.ElideRight
                }
            }

            Repeater {
                model: Document.outputs

                delegate: Item {
                    id: row

                    required property string name
                    required property bool isEnabled

                    objectName: "broadcastStatRow"
                    width: body.width
                    height: panel.wideEnough ? 17 : 28

                    Text {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.topMargin: panel.wideEnough ? 0 : 1
                        anchors.verticalCenter: panel.wideEnough
                            ? parent.verticalCenter : undefined
                        width: parent.width * (panel.wideEnough ? 0.30 : 1.0)
                        text: row.name
                        font.pixelSize: 10
                        color: Theme.c.text
                        elide: Text.ElideRight
                    }

                    // The narrow form: everything the columns would have said, on one line
                    // under the name.
                    Text {
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        visible: !panel.wideEnough
                        text: (row.isEnabled ? qsTr("Enabled") : qsTr("Off")) +
                              " · " + qsTr("dropped %1 · sent %2 · %3")
                                  .arg(panel.unavailable)
                                  .arg(panel.unavailable)
                                  .arg(panel.unavailable)
                        font.pixelSize: 9
                        color: Theme.c.textMuted
                        elide: Text.ElideRight
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.30
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width * 0.18
                        // "Inactive" would claim this was asked to run and is not. Nothing has
                        // been asked to run, so the honest word is that it is only configured.
                        text: row.isEnabled ? qsTr("Enabled") : qsTr("Off")
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.48
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.unavailable
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.70
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.unavailable
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }

                    Text {
                        visible: panel.wideEnough
                        anchors.left: parent.left
                        anchors.leftMargin: parent.width * 0.86
                        anchors.verticalCenter: parent.verticalCenter
                        text: panel.unavailable
                        font.pixelSize: 10
                        color: Theme.c.textMuted
                    }
                }
            }

            Text {
                width: parent.width
                visible: Document.outputs.rowCount() === 0
                text: qsTr("No outputs configured.")
                font.pixelSize: 10
                color: Theme.c.textMuted
                topPadding: 4
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

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("machine figures live · show figures need the renderer")
            font.pixelSize: 9
            color: Theme.c.textMuted
            opacity: 0.6
            elide: Text.ElideRight
        }

        ToolButton {
            anchors.right: parent.right
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            label: "↺"
            enabled: Preview.rendererAttached
            tip: Preview.rendererAttached
                ? qsTr("Reset per-canvas render counters")
                : qsTr("No renderer is attached")
            onClicked: Preview.resetStats()
        }
    }
}
