// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import Libertas

Item {
    id: panel
    objectName: "countdownsPanel"
    property int clockRevision: 0
    property string bindingId: ""
    Timer { interval: 200; running: true; repeat: true; onTriggered: panel.clockRevision++ }
    Flickable {
        anchors.fill: parent
        anchors.margins: 8
        contentWidth: width
        contentHeight: content.height
        clip: true
        interactive: false
        NoDragScroll {}
        Column {
            id: content
            width: parent.width
            spacing: 8
            DialogButton {
                text: qsTr("New countdown text")
                objectName: "newCountdown"
                onClicked: Document.addCountdown("")
            }
            ChoiceField {
                width: parent.width
                label: qsTr("Bind text")
                labelWidth: 80
                choices: { const revision = Document.revision; return Document.countdownTextSources().map(s => s.id); }
                choiceLabels: {
                    const revision = Document.revision;
                    const names = {};
                    for (const s of Document.countdownTextSources()) names[s.id] = s.name;
                    return names;
                }
                value: panel.bindingId
                onCommitted: value => panel.bindingId = value
            }
            DialogButton {
                text: qsTr("Add binding")
                enabled: panel.bindingId !== ""
                onClicked: Document.addCountdown(panel.bindingId)
            }
            DialogButton {
                text: qsTr("Import OBS timers…")
                onClicked: {
                    const path = FileDialog.pick(qsTr("OBS countdown config.json"), "", false);
                    if (path !== "") Document.importCountdowns(path);
                }
            }
            Text {
                width: parent.width
                visible: Document.countdowns.length === 0
                text: qsTr("Create countdown text or bind an existing text source. Add that source to each scene that should show it.")
                wrapMode: Text.WordWrap
                color: Theme.c.textMuted
                font.pixelSize: 11
            }
            Repeater {
                model: Document.countdowns
                delegate: Rectangle {
                    id: timer
                    required property var modelData
                    readonly property string sourceId: modelData.id
                    readonly property var status: { const tick = panel.clockRevision; return Document.countdownStatus(sourceId); }
                    function save(key, value) { const settings = {}; settings[key] = value; Document.configureCountdown(sourceId, settings); }
                    width: content.width
                    height: form.height + 16
                    color: Theme.c.panelHeader
                    radius: 3
                    Column {
                        id: form
                        x: 8; y: 8
                        width: parent.width - 16
                        spacing: 6
                        TextLine {
                            width: parent.width; label: qsTr("Name"); labelWidth: 100
                            value: timer.modelData.name
                            onCommitted: value => Document.renameSource(timer.sourceId, value)
                        }
                        Text {
                            text: timer.status.text || ""
                            color: Theme.c.text
                            font.pixelSize: 24
                            width: parent.width
                            wrapMode: Text.WrapAnywhere
                        }
                        Flow {
                            width: parent.width; spacing: 4
                            DialogButton {
                                objectName: "countdownStartPause"
                                text: timer.status.running ? qsTr("Pause") : qsTr("Start")
                                onClicked: Document.controlCountdown(timer.sourceId, timer.status.running ? "pause" : "start")
                            }
                            DialogButton { text: qsTr("Reset"); onClicked: Document.controlCountdown(timer.sourceId, "reset") }
                            DialogButton { text: qsTr("Remove binding"); onClicked: Document.removeCountdown(timer.sourceId) }
                        }
                        ChoiceField {
                            width: parent.width; label: qsTr("Mode"); labelWidth: 100
                            choices: ["remaining", "elapsed", "target"]
                            choiceLabels: ({"remaining": qsTr("Count down"), "elapsed": qsTr("Count up"), "target": qsTr("Target time")})
                            value: timer.modelData.mode || "remaining"
                            onCommitted: value => timer.save("mode", value)
                        }
                        NumberField {
                            width: parent.width; label: qsTr("Duration"); labelWidth: 100
                            decimals: 0; minimum: 0; suffix: qsTr("seconds")
                            value: (timer.modelData.durationMs === undefined ? 900000 : timer.modelData.durationMs) / 1000
                            onCommitted: value => timer.save("durationMs", Math.round(value * 1000))
                        }
                        TextLine {
                            width: parent.width; label: qsTr("Target (ISO)"); labelWidth: 100
                            visible: timer.modelData.mode === "target"
                            value: timer.modelData.targetAtMs ? new Date(Number(timer.modelData.targetAtMs)).toISOString() : ""
                            placeholder: "2026-10-22T20:00:00-05:00"
                            onCommitted: value => {
                                const date = Date.parse(value);
                                if (!isNaN(date)) timer.save("targetAtMs", date);
                                else NoticeLog.add(qsTr("Enter a valid ISO date and time with timezone."));
                            }
                        }
                        TextLine {
                            width: parent.width; label: qsTr("Format"); labelWidth: 100
                            value: timer.modelData.format === undefined ? "%time%" : timer.modelData.format
                            onCommitted: value => timer.save("format", value)
                        }
                        Flow {
                            width: parent.width; spacing: 10
                            Repeater {
                                model: ["showDays", "showHours", "showMinutes", "showSeconds", "leadingZero", "showEndMessage"]
                                delegate: CheckLabel {
                                    required property string modelData
                                    text: ({showDays: qsTr("Days"), showHours: qsTr("Hours"), showMinutes: qsTr("Minutes"), showSeconds: qsTr("Seconds"), leadingZero: qsTr("Leading zero"), showEndMessage: qsTr("End message")})[modelData]
                                    checked: timer.modelData[modelData] === undefined
                                        ? !["showDays", "showHours"].includes(modelData) : timer.modelData[modelData]
                                    onToggled: timer.save(modelData, !checked)
                                }
                            }
                        }
                        TextLine {
                            width: parent.width; label: qsTr("End message"); labelWidth: 100
                            value: timer.modelData.endMessage === undefined ? "SOON" : timer.modelData.endMessage
                            onCommitted: value => timer.save("endMessage", value)
                        }
                        ChoiceField {
                            width: parent.width; label: qsTr("End scene"); labelWidth: 100
                            choices: { const revision = Document.revision; return [""].concat(Document.countdownScenes().map(s => s.id)); }
                            choiceLabels: {
                                const revision = Document.revision;
                                const names = {"": qsTr("None")};
                                for (const s of Document.countdownScenes()) names[s.id] = s.name;
                                return names;
                            }
                            value: timer.modelData.endScene || ""
                            onCommitted: value => timer.save("endScene", value)
                        }
                    }
                }
            }
        }
    }
}
