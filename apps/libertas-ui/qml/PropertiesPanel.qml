// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

Item {
    id: panel

    readonly property var props: Document.layerProperties
    readonly property bool hasLayer: Object.keys(panel.props).length > 0

    Text {
        anchors.centerIn: parent
        visible: !panel.hasLayer
        text: qsTr("No layer selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Flickable {
        anchors.fill: parent
        visible: panel.hasLayer
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
                text: panel.hasLayer ? panel.props.name : ""
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.c.text
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: panel.hasLayer ? panel.props.kind : ""
                font.pixelSize: 10
                color: Theme.c.textMuted
                bottomPadding: 8
            }

            // --- Transform. Separate from Layout on purpose (ADR-0021): where it sits, versus
            // how the image fills its box. Conflating them is why OBS bounding boxes confuse.
            Text {
                width: parent.width
                text: qsTr("Transform")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 4
                bottomPadding: 4
            }

            NumberField {
                width: parent.width
                label: qsTr("X")
                value: panel.hasLayer ? panel.props.positionX : 0
                onCommitted: newValue => Document.setLayerPosition(newValue, panel.props.positionY)
            }

            NumberField {
                width: parent.width
                label: qsTr("Y")
                value: panel.hasLayer ? panel.props.positionY : 0
                onCommitted: newValue => Document.setLayerPosition(panel.props.positionX, newValue)
            }

            NumberField {
                width: parent.width
                label: qsTr("Scale X")
                decimals: 3
                value: panel.hasLayer ? panel.props.scaleX : 1
                onCommitted: newValue => Document.setLayerScale(newValue, panel.props.scaleY)
            }

            NumberField {
                width: parent.width
                label: qsTr("Scale Y")
                decimals: 3
                value: panel.hasLayer ? panel.props.scaleY : 1
                onCommitted: newValue => Document.setLayerScale(panel.props.scaleX, newValue)
            }

            NumberField {
                width: parent.width
                label: qsTr("Rotation")
                decimals: 1
                value: panel.hasLayer ? panel.props.rotation : 0
                onCommitted: newValue => Document.setLayerRotation(newValue)
            }

            // --- Blending
            Text {
                width: parent.width
                text: qsTr("Blending")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            NumberField {
                width: parent.width
                label: qsTr("Opacity")
                decimals: 3
                value: panel.hasLayer ? panel.props.opacity : 1
                onCommitted: newValue => Document.setLayerOpacity(newValue)
            }

            ChoiceField {
                width: parent.width
                label: qsTr("Mode")
                choices: Document.blendModes
                value: panel.hasLayer ? panel.props.blendMode : ""
                onCommitted: newValue => Document.setLayerBlendMode(newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.hasLayer &&
                         ["hue", "saturation", "color", "luminosity"].indexOf(panel.props.blendMode) >= 0
                text: qsTr("This mode needs the backdrop, so the group containing it is flattened to an intermediate before compositing.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 4
            }

            // --- State
            Text {
                width: parent.width
                text: qsTr("State")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            Row {
                spacing: 14

                CheckLabel {
                    text: qsTr("Visible")
                    checked: panel.hasLayer && panel.props.visible
                    onToggled: Document.setLayerVisible(Document.selectedLayer,
                                                        !panel.props.visible)
                }

                CheckLabel {
                    text: qsTr("Locked")
                    checked: panel.hasLayer && panel.props.locked
                    onToggled: Document.setLayerLocked(Document.selectedLayer,
                                                       !panel.props.locked)
                }
            }

            Text {
                width: parent.width
                visible: panel.hasLayer && panel.props.effectCount > 0
                text: qsTr("%1 effect(s)").arg(panel.hasLayer ? panel.props.effectCount : 0)
                font.pixelSize: 10
                color: Theme.c.textMuted
                topPadding: 10
            }
        }
    }
}
