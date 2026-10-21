// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import Libertas

Item {
    id: dialog
    objectName: "filtersDialog"
    anchors.fill: parent
    z: 10000
    visible: false
    property string owner: ""
    property string selected: ""
    property var details: ({})
    property var current: ({})
    property var types: []
    property string addType: ""
    function refresh() {
        details = Document.filterDetails(owner);
        if (!details.owner) { visible = false; return; }
        const list = details.effects || [];
        current = list.find(e => e.id === selected) || list[0] || {};
        selected = current.id || "";
        types = Document.filterTypes(owner);
        if (!types.some(t => t.type === addType)) addType = types.length ? types[0].type : "";
    }
    function open(id) { owner = id; selected = ""; refresh(); visible = !!details.owner; forceActiveFocus(); }
    function edit(key, value) {
        const values = Object.assign({}, current.values);
        values[key] = value;
        Document.editFilter(owner, selected, current.name, current.enabled, values);
    }
    Connections { target: Document; function onChanged() { if (dialog.visible) dialog.refresh(); } }
    Keys.onEscapePressed: visible = false
    Rectangle { anchors.fill: parent; color: "#80000000" }
    MouseArea { anchors.fill: parent; onClicked: dialog.visible = false; onWheel: wheel => { wheel.accepted = true; } }
    Rectangle {
        id: frame
        width: Math.min(parent.width - 32, 850)
        height: Math.min(parent.height - 32, 640)
        anchors.centerIn: parent
        color: Theme.c.panelBackground
        border.color: Theme.c.accent
        MouseArea { anchors.fill: parent; onClicked: {} }
        Text { x: 16; y: 12; text: qsTr("Filters — %1").arg(dialog.details.name || ""); color: Theme.c.text; font.pixelSize: 16 }
        Text { x: 16; y: 38; text: qsTr("Changes apply to every use of this source. Undo restores edits."); color: Theme.c.textMuted; font.pixelSize: 11 }
        ListView {
            id: list
            objectName: "filterList"
            x: 12; y: 64; width: 240; height: frame.height - 164
            clip: true
            interactive: false
            NoDragScroll {}
            model: dialog.details.effects || []
            spacing: 4
            delegate: Rectangle {
                id: entry
                required property var modelData
                width: list.width; height: 36
                color: dialog.selected === modelData.id ? Theme.c.panelHeaderActive : "transparent"
                Text { x: 8; anchors.verticalCenter: parent.verticalCenter; width: parent.width - 16; elide: Text.ElideRight; text: (entry.modelData.enabled ? "" : qsTr("[Off] ")) + entry.modelData.name; color: Theme.c.text }
                MouseArea { anchors.fill: parent; onClicked: { dialog.selected = entry.modelData.id; dialog.refresh(); } }
            }
        }
        Row {
            x: 12; y: frame.height - 92; spacing: 5
            DialogButton { objectName: "filterUp"; text: qsTr("Up"); enabled: !!dialog.selected; onClicked: Document.moveFilter(dialog.owner, dialog.selected, -1) }
            DialogButton { objectName: "filterDown"; text: qsTr("Down"); enabled: !!dialog.selected; onClicked: Document.moveFilter(dialog.owner, dialog.selected, 1) }
            DialogButton { objectName: "filterRemove"; text: qsTr("Remove"); enabled: !!dialog.selected; onClicked: Document.removeFilter(dialog.owner, dialog.selected) }
        }
        Flickable {
            interactive: false
            NoDragScroll {}
            x: 270; y: 64; width: frame.width - 286; height: frame.height - 120
            clip: true; contentHeight: settings.height
            Column {
                id: settings
                width: parent.width; spacing: 8
                visible: !!dialog.selected
                Text { text: qsTr("Name"); color: Theme.c.textMuted }
                Rectangle {
                    width: parent.width; height: 30; color: Theme.c.window; border.color: Theme.c.border
                    TextInput {
                        objectName: "filterName"
                        anchors.fill: parent; anchors.margins: 5
                        text: dialog.current.name || ""; color: Theme.c.text; selectByMouse: true
                        onEditingFinished: Document.editFilter(dialog.owner, dialog.selected, text, dialog.current.enabled, dialog.current.values)
                    }
                }
                CheckLabel {
                    objectName: "filterEnabled"
                    width: parent.width; height: 24; text: qsTr("Enabled")
                    checked: dialog.current.enabled === true
                    onToggled: Document.editFilter(dialog.owner, dialog.selected, dialog.current.name, !checked, dialog.current.values)
                }
                Text { width: parent.width; wrapMode: Text.WordWrap; text: dialog.current.status || ""; visible: text !== ""; color: Theme.c.textMuted }
                Repeater {
                    model: dialog.current.schema || []
                    delegate: SettingRow {
                        required property var modelData
                        width: settings.width; field: modelData; currentValue: modelData.value
                        onCommitted: value => dialog.edit(modelData.key, value)
                    }
                }
            }
        }
        ChoiceField {
            objectName: "filterType"
            x: 12; y: frame.height - 50; width: frame.width - 200
            label: qsTr("Add filter"); labelWidth: 70
            choices: dialog.types.map(t => t.type)
            choiceLabels: { const labels = {}; for (const t of dialog.types) labels[t.type] = t.name; return labels; }
            value: dialog.addType
            onCommitted: value => dialog.addType = value
        }
        Row {
            anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 12; spacing: 8
            DialogButton { objectName: "filterAdd"; text: qsTr("Add"); enabled: dialog.addType !== ""; onClicked: { Document.addFilter(dialog.owner, dialog.addType); const entries = dialog.details.effects; if (entries.length) dialog.selected = entries[entries.length - 1].id; dialog.refresh(); } }
            DialogButton { text: qsTr("Close"); onClicked: dialog.visible = false }
        }
    }
}
