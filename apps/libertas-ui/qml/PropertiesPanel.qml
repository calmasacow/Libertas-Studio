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

            // --- Layout. Separate from Transform, and the separation is the point: where the
            // box sits versus how the content fills it. Conflating them is why OBS bounding
            // boxes confuse people -- scaling a source and changing its fit mode look like the
            // same operation until they disagree.
            Text {
                width: parent.width
                text: qsTr("Layout")
                font.pixelSize: 10
                font.weight: Font.DemiBold
                color: Theme.c.accent
                topPadding: 10
                bottomPadding: 4
            }

            ChoiceField {
                width: parent.width
                label: qsTr("Fit")
                choices: Document.fitModes
                value: panel.hasLayer ? panel.props.fit : ""
                onCommitted: newValue => Document.setLayerFit(newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.hasLayer && panel.props.fit === "natural"
                text: qsTr("Natural size: the box takes the source's own dimensions and no fitting happens.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 2
                bottomPadding: 2
            }

            NumberField {
                width: parent.width
                label: qsTr("Box W")
                decimals: 0
                enabled: panel.hasLayer && panel.props.fit !== "natural"
                value: panel.hasLayer ? panel.props.boxWidth : 0
                onCommitted: newValue => Document.setLayerBoxSize(newValue, panel.props.boxHeight)
            }

            NumberField {
                width: parent.width
                label: qsTr("Box H")
                decimals: 0
                enabled: panel.hasLayer && panel.props.fit !== "natural"
                value: panel.hasLayer ? panel.props.boxHeight : 0
                onCommitted: newValue => Document.setLayerBoxSize(panel.props.boxWidth, newValue)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop L")
                decimals: 0
                value: panel.hasLayer ? panel.props.cropLeft : 0
                onCommitted: newValue => Document.setLayerCrop(newValue, panel.props.cropTop,
                                                               panel.props.cropRight,
                                                               panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop T")
                decimals: 0
                value: panel.hasLayer ? panel.props.cropTop : 0
                onCommitted: newValue => Document.setLayerCrop(panel.props.cropLeft, newValue,
                                                               panel.props.cropRight,
                                                               panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop R")
                decimals: 0
                value: panel.hasLayer ? panel.props.cropRight : 0
                onCommitted: newValue => Document.setLayerCrop(panel.props.cropLeft,
                                                               panel.props.cropTop, newValue,
                                                               panel.props.cropBottom)
            }

            NumberField {
                width: parent.width
                label: qsTr("Crop B")
                decimals: 0
                value: panel.hasLayer ? panel.props.cropBottom : 0
                onCommitted: newValue => Document.setLayerCrop(panel.props.cropLeft,
                                                               panel.props.cropTop,
                                                               panel.props.cropRight, newValue)
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: panel.hasLayer &&
                         (panel.props.cropLeft > 0 || panel.props.cropTop > 0 ||
                          panel.props.cropRight > 0 || panel.props.cropBottom > 0)
                text: qsTr("Crop is in source pixels and is taken off before fitting.")
                font.pixelSize: 9
                color: Theme.c.textMuted
                topPadding: 2
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
