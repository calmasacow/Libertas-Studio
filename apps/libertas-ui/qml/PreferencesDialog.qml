// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import Libertas

/// Edit > Preferences: settings for this computer, not the collection.
Window {
    id: dialog
    objectName: "preferencesDialog"
    title: qsTr("Preferences")
    width: 560; height: 250
    minimumWidth: 480; minimumHeight: 250
    color: Theme.c.window
    modality: Qt.ApplicationModal
    flags: Qt.Dialog | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
    property bool automatic: true
    property int budgetMib: 2048
    property string problem: ""
    function open() {
        automatic = Preferences.gpuBudgetMib === 0;
        budgetMib = automatic ? 2048 : Preferences.gpuBudgetMib;
        problem = "";
        show();
        requestActivate();
    }
    Column {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        Text {
            text: qsTr("GPU memory")
            color: Theme.c.text
            font.pixelSize: 13
            font.bold: true
        }
        CheckLabel {
            objectName: "gpuBudgetAutomatic"
            text: qsTr("Automatic: half of the GPU memory free when the renderer starts")
            checked: dialog.automatic
            onToggled: dialog.automatic = !dialog.automatic
        }
        NumberField {
            objectName: "gpuBudgetMib"
            width: parent.width; label: qsTr("Compositor limit"); labelWidth: 140
            enabled: !dialog.automatic
            opacity: enabled ? 1 : 0.5
            decimals: 0; minimum: 256; maximum: 65536; suffix: "MiB"
            value: dialog.budgetMib
            onCommitted: value => dialog.budgetMib = Math.round(value)
        }
        Text {
            width: parent.width
            text: qsTr("Images, effects and decoded frames held on the GPU. A show that exceeds it stops compositing on the GPU. Applying restarts the renderer, which briefly blanks the preview and program.")
            color: Theme.c.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: 11
        }
        Text {
            visible: dialog.problem !== ""
            text: dialog.problem
            color: Theme.c.error
            font.pixelSize: 11
        }
        Row {
            spacing: 8
            DialogButton {
                text: qsTr("Apply"); primary: true
                objectName: "applyPreferences"
                onClicked: {
                    if (Preferences.setGpuBudgetMib(dialog.automatic ? 0 : dialog.budgetMib))
                        dialog.close();
                    else
                        dialog.problem = qsTr("Could not save: enter 256 to 65536 MiB.");
                }
            }
            DialogButton { text: qsTr("Cancel"); onClicked: dialog.close() }
        }
    }
}
