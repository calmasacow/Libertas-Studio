// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs
import Libertas

/// What one source is configured to be: the gear at the bottom of the Sources panel, and a
/// double-click on a row.
///
/// The shape is the one OBS gets right and the operator asked for -- a preview above, the
/// fields below it, Defaults on the left of the bottom bar and OK/Cancel on the right -- but
/// the fields are not hand-built per type. They come from `Document.sourceSettingsSchema`, so
/// a new setting on a source type is a line in a table in C++ and nothing in this file.
///
/// Nothing is committed while the panel is open. Every edit goes into `pending`, and OK sends
/// the lot as one `SetSource`, which is one undo step named for the source. That is what makes
/// Cancel honest: there is nothing to put back, because nothing was done. The alternative --
/// committing per field and reverting on Cancel -- leaves the abandoned edits on the undo stack
/// for the operator to press Ctrl+Z through later.
///
/// The preview is honest, the same rule the add dialog follows. A colour is the one source this
/// build can draw correctly with no renderer, so a colour shows the colour being typed, live.
/// Everything else says there is no pipeline attached, because there is not.
///
/// Drawn inside the workspace rather than as its own window (ADR-0015). The file chooser is the
/// exception and is meant to be: that is the portal's dialog, run by the desktop, and it is the
/// only way a Wayland client gets a path the sandbox will honour.
Item {
    id: dialog

    /// The source being configured. Empty when the panel is closed.
    property string sourceId: ""
    property var details: ({})
    property var schema: []
    /// Edits made since the panel opened, by settings key. Replaced wholesale on every change,
    /// because assigning into a var object in place notifies nothing.
    property var pending: ({})
    property string pendingName: ""
    /// Which path field a Browse press came from.
    property string browsingKey: ""

    readonly property bool known: Object.keys(dialog.details).length > 0
    readonly property string typeId: dialog.known ? dialog.details.type : ""

    signal closed

    anchors.fill: parent
    visible: false
    focus: visible

    function open(id) {
        const found = Document.sourceDetails(id);
        if (Object.keys(found).length === 0) {
            NoticeLog.add(qsTr("That source is no longer in the project."));
            return;
        }
        dialog.sourceId = id;
        dialog.details = found;
        dialog.schema = Document.sourceSettingsSchema(found.type);
        dialog.pending = Object.assign({}, found.settings);
        dialog.pendingName = found.name;
        dialog.visible = true;
        dialog.forceActiveFocus();
    }

    function close() {
        dialog.visible = false;
        dialog.sourceId = "";
        dialog.details = ({});
        dialog.schema = [];
        dialog.pending = ({});
        dialog.closed();
    }

    function setValue(key, value) {
        const next = Object.assign({}, dialog.pending);
        next[key] = value;
        dialog.pending = next;
    }

    /// Back to what a brand-new source of this type would be. The name is left alone: nobody
    /// pressing Defaults on a camera's settings means "and call it Camera again".
    function restoreDefaults() {
        const next = {};
        for (const f of dialog.schema) {
            if (f.kind !== "note")
                next[f.key] = f.value;
        }
        dialog.pending = next;
    }

    function accept() {
        Document.applySourceSettings(dialog.sourceId, dialog.pendingName, dialog.pending);
        dialog.close();
    }

    Keys.onEscapePressed: dialog.close()

    FileDialog {
        id: picker

        title: qsTr("Choose a file")
        onAccepted: {
            // A local path, not a URL: it is what goes in the project, and what the worker
            // process will eventually be asked to open.
            const path = decodeURIComponent(picker.selectedFile.toString()
                                                  .replace(/^file:\/\//, ""));
            dialog.setValue(dialog.browsingKey, path);
        }
    }

    // The workspace behind is inert while this is up, and clicking away closes it.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: dialog.close()
        onWheel: (wheel) => { wheel.accepted = true; }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.window
        opacity: 0.55
    }

    Rectangle {
        id: frame

        objectName: "sourceSettingsFrame"
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 760)
        height: Math.min(parent.height - 48, 660)
        color: Theme.c.panelBackground
        border.width: 1
        border.color: Theme.c.accent
        radius: 3

        MouseArea {
            anchors.fill: parent
            onClicked: {}
        }

        // ---- the title ----------------------------------------------------------------
        Item {
            id: titleBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: 34

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 13
                anchors.right: typeBadge.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: dialog.known ? qsTr("Settings — %1").arg(dialog.pendingName) : ""
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.c.text
                elide: Text.ElideRight
            }

            Row {
                id: typeBadge

                anchors.right: parent.right
                anchors.rightMargin: 13
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // ADR-0025: safe to hold a safety net, said where the operator is already
                // looking at what this source is.
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: dialog.known && dialog.details.selfContained
                    text: "⛨"
                    font.pixelSize: 11
                    color: Theme.c.good
                    opacity: 0.8
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: dialog.known ? dialog.details.typeTitle : ""
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
            }
        }

        // ---- the preview ---------------------------------------------------------------
        Rectangle {
            id: preview

            objectName: "sourceSettingsPreview"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: titleBar.bottom
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            height: Math.round(Math.min(frame.height * 0.3, (frame.width - 24) * 9 / 16))
            color: Theme.c.window
            border.width: 1
            border.color: Theme.c.border
            radius: 2
            clip: true

            // The one thing this build can draw correctly with no renderer, and it tracks what
            // is being typed rather than what is stored -- which is the whole use of a preview.
            //
            // Letterboxed to the card's own size rather than filling the box, so that the width
            // and height below are visibly the thing they say they are.
            Rectangle {
                readonly property real aspect: {
                    const w = Number(dialog.pending["width"]);
                    const h = Number(dialog.pending["height"]);
                    return (w > 0 && h > 0) ? w / h : 16 / 9;
                }

                anchors.centerIn: parent
                visible: dialog.typeId === "source.color"
                width: Math.min(parent.width - 2, (parent.height - 2) * aspect)
                height: Math.min(parent.height - 2, (parent.width - 2) / aspect)
                color: {
                    const hex = dialog.pending["color"];
                    return /^#[0-9a-fA-F]{6}$/.test(hex === undefined ? "" : String(hex))
                         ? String(hex) : "transparent";
                }
            }

            Column {
                anchors.centerIn: parent
                visible: dialog.typeId !== "source.color"
                spacing: 4

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: dialog.known ? dialog.details.icon : ""
                    font.pixelSize: 26
                    color: Theme.c.textMuted
                    opacity: 0.75
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("no pipeline attached")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                    opacity: 0.7
                }
            }
        }

        // ---- the fields ----------------------------------------------------------------
        Flickable {
            id: scroll

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: preview.bottom
            anchors.bottom: bottomBar.top
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.topMargin: 10
            anchors.bottomMargin: 8
            contentHeight: fields.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: fields

                width: scroll.width
                spacing: 4

                // The source's own name, which is a different thing from the layer's: one
                // camera can sit in six scenes under six labels, and this is the camera.
                TextLine {
                    objectName: "sourceNameField"
                    width: parent.width
                    label: qsTr("Name")
                    labelWidth: 150
                    labelAlign: Text.AlignRight
                    value: dialog.pendingName
                    onCommitted: newValue => dialog.pendingName = newValue
                }

                Text {
                    width: parent.width
                    text: dialog.known
                        ? (dialog.details.useCount === 1
                            ? qsTr("Used by one layer. Changing it changes that layer.")
                            : qsTr("Used by %1 layers. Changing it changes all of them.")
                                  .arg(dialog.details.useCount))
                        : ""
                    visible: dialog.known && dialog.details.useCount > 0
                    leftPadding: 154
                    bottomPadding: 6
                    font.pixelSize: 10
                    color: dialog.known && dialog.details.useCount > 1
                         ? Theme.c.warning : Theme.c.textMuted
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: dialog.schema

                    delegate: SettingRow {
                        id: settingRow

                        required property var modelData

                        objectName: "sourceSettingRow"
                        width: fields.width
                        field: settingRow.modelData
                        labelWidth: 150
                        currentValue: dialog.pending[settingRow.modelData.key]
                        onCommitted: newValue =>
                            dialog.setValue(settingRow.modelData.key, newValue)
                        onBrowseRequested: {
                            dialog.browsingKey = settingRow.modelData.key;
                            picker.open();
                        }
                    }
                }

                // A type this build has no field table for -- a project written by a later
                // build. Said plainly, because an empty panel looks like a broken one.
                Text {
                    width: parent.width
                    visible: dialog.schema.length === 0
                    topPadding: 24
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("This build has no settings for a %1 source.")
                              .arg(dialog.known ? dialog.details.typeTitle : "")
                    font.pixelSize: 11
                    color: Theme.c.textMuted
                }
            }
        }

        // ---- the bottom bar -------------------------------------------------------------
        Rectangle {
            id: bottomBar

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 1
            height: 44
            color: Theme.c.panelHeader

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Theme.c.border
            }

            DialogButton {
                objectName: "sourceDefaultsButton"
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Defaults")
                onClicked: dialog.restoreDefaults()
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                DialogButton {
                    objectName: "sourceSettingsOkButton"
                    text: qsTr("OK")
                    primary: true
                    onClicked: dialog.accept()
                }

                DialogButton {
                    objectName: "sourceSettingsCancelButton"
                    text: qsTr("Cancel")
                    onClicked: dialog.close()
                }
            }
        }
    }
}
