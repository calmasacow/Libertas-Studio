// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// What is in the project, and what is wrong with it.
///
/// ADR-0020 is why there is no single "frame time" here waiting to be filled in: per-output and
/// per-canvas figures have to be reported separately, because a hitch in one canvas must not be
/// hidden inside another's number. The pipeline rows arrive with the pipeline.
Item {
    id: panel

    Flickable {
        anchors.fill: parent
        contentHeight: body.implicitHeight + 20
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: body

            x: 10
            y: 10
            width: parent.width - 20
            spacing: 2

            Text {
                width: parent.width
                text: qsTr("Document")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                bottomPadding: 4
            }

            StatRow {
                width: parent.width
                label: qsTr("Scenes")
                value: Document.scenes.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Layers here")
                value: Document.layers.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Inputs")
                value: Document.sources.rowCount()
            }

            StatRow {
                width: parent.width
                label: qsTr("Edits")
                value: Document.revision
            }

            StatRow {
                width: parent.width
                label: qsTr("Saved")
                value: Document.dirty ? qsTr("no") : qsTr("yes")
                warn: Document.dirty
            }

            Text {
                width: parent.width
                text: qsTr("Problems")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 12
                bottomPadding: 4
            }

            Text {
                width: parent.width
                visible: Document.problems.length === 0
                text: qsTr("None.")
                font.pixelSize: 11
                color: Theme.c.textMuted
            }

            Repeater {
                model: Document.problems

                delegate: Text {
                    required property string modelData

                    width: body.width
                    text: modelData
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    color: modelData.startsWith("error") ? Theme.c.error : Theme.c.warning
                    bottomPadding: 3
                }
            }

            Text {
                width: parent.width
                text: qsTr("Pipeline")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 12
                bottomPadding: 4
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("No pipeline attached. When there is one, frame times are reported per output and per canvas rather than as a single number — a hitch in one canvas must not be hidden inside another's.")
                font.pixelSize: 10
                color: Theme.c.textMuted
            }
        }
    }
}
