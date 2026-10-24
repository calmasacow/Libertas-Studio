// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

pragma ComponentBehavior: Bound

import QtQuick
import Libertas

/// The settings for whatever is selected on the graph.
///
/// Selection is view state, not document state: the graph holds it and hands it here. Nothing
/// about which node someone is looking at belongs in a file that goes to another machine.
///
/// Every field edits one node. With nothing selected the column says so rather than showing
/// the first one, for the same reason PropertiesPanel does (ADR-0037).
Item {
    id: panel

    required property string selectedKind
    required property string selectedId

    readonly property var encode: panel.selectedKind === "encode"
                                  ? Document.encodeSettings(panel.selectedId) : ({})
    readonly property var audio: panel.selectedKind === "audio-encode"
                                 ? Document.audioEncodeSettings(panel.selectedId) : ({})
    readonly property var extra: panel.encode.extra || ({})
    readonly property string implementation: panel.encode.implementation || ""

    // Document emits `changed` after every command, and these are calls rather than
    // properties, so without this they would never re-read.
    property int revision: 0
    Connections {
        target: Document
        function onChanged() { panel.revision++; }
    }

    function setExtra(key, value) {
        Document.setEncodeExtra(panel.selectedId, key, value);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.c.panelBackground
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.c.border
    }

    Text {
        anchors.centerIn: parent
        visible: panel.selectedKind === ""
        text: qsTr("No node selected")
        font.pixelSize: 11
        color: Theme.c.textMuted
    }

    Flickable {
        anchors.fill: parent
        anchors.leftMargin: 1
        visible: panel.selectedKind !== ""
        contentHeight: body.implicitHeight + 24
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Nothing scrolls by being dragged (ADR-0039); the wheel has to come back because
        // Flickable drops it along with the drag.
        interactive: false

        NoDragScroll {}

        Column {
            id: body

            x: 14
            y: 12
            width: parent.width - 28
            spacing: 6

            Text {
                width: parent.width
                text: {
                    void panel.revision;
                    if (panel.selectedKind === "encode")
                        return qsTr("Properties: %1").arg(panel.encode.name || "");
                    if (panel.selectedKind === "audio-encode")
                        return qsTr("Properties: %1").arg(panel.audio.name || "");
                    if (panel.selectedKind === "canvas")
                        return qsTr("Properties: %1").arg(
                            Document.canvasNameOf(panel.selectedId));
                    if (panel.selectedKind === "output" || panel.selectedKind === "bus")
                        return qsTr("Properties: %1").arg(nodeName.value);
                    return qsTr("Properties");
                }
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.c.accent
                elide: Text.ElideRight
                bottomPadding: 6
            }

            // Name is looked up from the models for the kinds that have no settings call of
            // their own, rather than adding two more invokables for one string each.
            QtObject {
                id: nodeName

                readonly property string value: {
                    void panel.revision;
                    if (panel.selectedKind === "output") {
                        const all = Document.encodeChoices();
                        for (let i = 0; i < all.length; ++i)
                            for (let j = 0; j < all[i].destinations.length; ++j)
                                if (all[i].destinations[j].id === panel.selectedId)
                                    return all[i].destinations[j].name;
                        return "";
                    }
                    if (panel.selectedKind === "bus") {
                        const buses = Document.audioBusChoices();
                        for (let i = 0; i < buses.length; ++i)
                            if (buses[i].id === panel.selectedId)
                                return buses[i].name;
                    }
                    return "";
                }
            }

            // --- a video encode ---------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "encode"

                TextLine {
                    width: parent.width
                    label: qsTr("Encoder Name")
                    labelWidth: 150
                    value: panel.encode.name || ""
                    onCommitted: value => Document.renameEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Rate Control")
                    labelWidth: 150
                    choices: panel.implementation === "x264" ? ["cbr", "vbr", "crf"]
                           : panel.implementation === "nvenc" ? ["cbr", "vbr", "cqp"]
                           : ["cbr", "vbr"]
                    choiceLabels: ({"cbr": qsTr("Constant Bitrate"),
                                    "vbr": qsTr("Variable Bitrate"),
                                    "cqp": qsTr("Constant QP"),
                                    "crf": qsTr("Constant Quality")})
                    value: panel.encode.rateControl || "cbr"
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, value, panel.encode.preset || "",
                        panel.encode.keyframeMs || 2000)
                }

                NumberField {
                    width: parent.width
                    label: qsTr("Bitrate")
                    labelWidth: 150
                    decimals: 0
                    suffix: qsTr("Kbps")
                    steppers: true
                    step: 500
                    visible: !["cqp", "crf"].includes(panel.encode.rateControl || "cbr")
                    value: panel.encode.bitrateKbps || 0
                    onCommitted: value => Document.setEncodeBitrate(
                        panel.selectedId, Math.max(1, Math.round(value)))
                }

                NumberField {
                    width: parent.width
                    label: qsTr("Keyframe Interval")
                    labelWidth: 150
                    decimals: 0
                    suffix: qsTr("s")
                    steppers: true
                    value: (panel.encode.keyframeMs || 0) / 1000
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, panel.encode.rateControl || "cbr",
                        panel.encode.preset || "", Math.round(value * 1000))
                }

                ChoiceField {
                    width: parent.width
                    label: panel.implementation === "x264" ? qsTr("CPU Usage Preset")
                                                           : qsTr("Preset")
                    labelWidth: 150
                    choices: panel.implementation === "nvenc"
                             ? ["p1", "p2", "p3", "p4", "p5", "p6", "p7"]
                           : panel.implementation === "x264"
                             ? ["", "ultrafast", "superfast", "veryfast", "faster", "fast",
                                "medium", "slow", "slower", "veryslow", "placebo"]
                           : [""]
                    choiceLabels: ({"": qsTr("Encoder default"),
                                    "p1": "P1: Fastest", "p2": "P2: Faster", "p3": "P3: Fast",
                                    "p4": "P4: Medium", "p5": "P5: Slow (Good Quality)",
                                    "p6": "P6: Slower", "p7": "P7: Slowest (Best Quality)"})
                    value: panel.encode.preset || ""
                    onCommitted: value => Document.setEncodeOptions(
                        panel.selectedId, panel.encode.rateControl || "cbr", value,
                        panel.encode.keyframeMs || 2000)
                }

                // The nvenc knobs. These live in EncoderConfig::extra, which is where the
                // outputs manager already drives them from -- same keys, same meanings.
                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    label: qsTr("Tuning")
                    labelWidth: 150
                    choices: ["hq", "ll", "ull", "lossless"]
                    choiceLabels: ({"hq": qsTr("High Quality"), "ll": qsTr("Low Latency"),
                                    "ull": qsTr("Ultra Low Latency"),
                                    "lossless": qsTr("Lossless")})
                    value: String(panel.extra["nvenc.tuning"] ?? "hq")
                    onCommitted: value => panel.setExtra("nvenc.tuning", value)
                }

                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    label: qsTr("Multipass Mode")
                    labelWidth: 150
                    choices: ["disabled", "quarter", "full"]
                    choiceLabels: ({"disabled": qsTr("Single Pass"),
                                    "quarter": qsTr("Two Passes (Quarter Resolution)"),
                                    "full": qsTr("Two Passes (Full Resolution)")})
                    value: String(panel.extra["nvenc.multipass"] ?? "quarter")
                    onCommitted: value => panel.setExtra("nvenc.multipass", value)
                }

                ChoiceField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    label: qsTr("Profile")
                    labelWidth: 150
                    choices: (panel.encode.codec === "h264") ? ["baseline", "main", "high"]
                           : (panel.encode.codec === "hevc") ? ["main", "main10"] : ["main"]
                    choiceLabels: ({"baseline": qsTr("Baseline"), "main": qsTr("Main"),
                                    "high": qsTr("High"), "main10": qsTr("Main 10")})
                    value: String(panel.extra["nvenc.profile"] ??
                                  (panel.encode.codec === "h264" ? "high" : "main"))
                    onCommitted: value => panel.setExtra("nvenc.profile", value)
                }

                Item { width: 1; height: 4 }

                CheckLabel {
                    visible: panel.implementation === "nvenc"
                    text: qsTr("Look Ahead")
                    checked: panel.extra["nvenc.lookahead"] === true
                    onToggled: panel.setExtra("nvenc.lookahead",
                                              !(panel.extra["nvenc.lookahead"] === true))
                }

                CheckLabel {
                    visible: panel.implementation === "nvenc"
                    text: qsTr("Adaptive Quantization")
                    checked: panel.extra["nvenc.adaptive_quantization"] !== false
                    onToggled: panel.setExtra(
                        "nvenc.adaptive_quantization",
                        !(panel.extra["nvenc.adaptive_quantization"] !== false))
                }

                Item { width: 1; height: 4 }

                NumberField {
                    width: parent.width
                    visible: panel.implementation === "nvenc"
                    label: qsTr("B-Frames")
                    labelWidth: 150
                    decimals: 0
                    steppers: true
                    minimum: 0
                    maximum: 16
                    value: Number(panel.extra["nvenc.bframes"] ?? 2)
                    onCommitted: value => panel.setExtra("nvenc.bframes", Math.round(value))
                }

                // Which device, not which encoder. The list of devices a machine really has is
                // `libertas probe`'s answer and is not wired up yet, so this says so rather
                // than offering a menu of one invented entry.
                TextLine {
                    width: parent.width
                    label: qsTr("GPU Selection")
                    labelWidth: 150
                    placeholder: Telemetry.gpuAvailable ? Telemetry.gpuName
                                                        : qsTr("default device")
                    value: String(panel.extra["gpu"] ?? "")
                    onCommitted: value => panel.setExtra("gpu", value)
                }

                TextLine {
                    width: parent.width
                    label: qsTr("Encoder Options")
                    labelWidth: 150
                    value: String(panel.extra[panel.implementation === "x264"
                                              ? "x264.options" : "nvenc.options"] ?? "")
                    onCommitted: value => panel.setExtra(
                        panel.implementation === "x264" ? "x264.options" : "nvenc.options",
                        value)
                }
            }

            // --- an audio encode --------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "audio-encode"

                TextLine {
                    width: parent.width
                    label: qsTr("Encoder Name")
                    labelWidth: 150
                    value: panel.audio.name || ""
                    onCommitted: value => Document.renameAudioEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Audio Track")
                    labelWidth: 150
                    choices: {
                        void panel.revision;
                        return Document.audioBusChoices().map(b => b.id);
                    }
                    choiceLabels: {
                        void panel.revision;
                        const out = {};
                        for (const b of Document.audioBusChoices())
                            out[b.id] = b.name;
                        return out;
                    }
                    value: panel.audio.busId || ""
                    onCommitted: value => Document.setAudioEncodeBus(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Codec")
                    labelWidth: 150
                    choices: ["aac", "opus"]
                    choiceLabels: ({"aac": "AAC", "opus": "Opus"})
                    value: panel.audio.codec || "aac"
                    onCommitted: value => Document.setAudioEncodeCodec(panel.selectedId, value)
                }

                NumberField {
                    width: parent.width
                    label: qsTr("Bitrate")
                    labelWidth: 150
                    decimals: 0
                    suffix: qsTr("Kbps")
                    steppers: true
                    step: 32
                    minimum: 1
                    value: panel.audio.bitrateKbps || 0
                    onCommitted: value => Document.setAudioEncodeBitrate(
                        panel.selectedId, Math.max(1, Math.round(value)))
                }
            }

            // --- a destination ----------------------------------------------------------
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "output"

                TextLine {
                    width: parent.width
                    label: qsTr("Name")
                    labelWidth: 150
                    value: nodeName.value
                    onCommitted: value => Document.renameOutput(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Video Encoder")
                    labelWidth: 150
                    choices: {
                        void panel.revision;
                        return Document.encodeChoices().map(e => e.id);
                    }
                    choiceLabels: {
                        void panel.revision;
                        const out = {};
                        for (const e of Document.encodeChoices())
                            out[e.id] = e.name;
                        return out;
                    }
                    value: {
                        void panel.revision;
                        return Document.outputEncoderSettings(panel.selectedId).id || "";
                    }
                    onCommitted: value => Document.setOutputEncode(panel.selectedId, value)
                }

                ChoiceField {
                    width: parent.width
                    label: qsTr("Audio Encoder")
                    labelWidth: 150
                    choices: {
                        void panel.revision;
                        return [""].concat(Document.audioEncodeChoices().map(e => e.id));
                    }
                    choiceLabels: {
                        void panel.revision;
                        const out = {"": qsTr("None (silent)")};
                        for (const e of Document.audioEncodeChoices())
                            out[e.id] = e.name;
                        return out;
                    }
                    value: outputAudio.value
                    onCommitted: value => Document.setOutputAudioEncode(panel.selectedId, value)
                }

                QtObject {
                    id: outputAudio

                    readonly property string value: {
                        void panel.revision;
                        return Document.outputAudioEncode(panel.selectedId);
                    }
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    visible: outputAudio.value === ""
                    text: qsTr("This destination carries no audio and will go out silent.")
                    font.pixelSize: 9
                    color: Theme.c.warning
                }

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("Stream keys are never stored in the project. Set one through "
                             + "the outputs manager; the project holds only its name.")
                    font.pixelSize: 9
                    color: Theme.c.textMuted
                    topPadding: 8
                }
            }

            // --- a canvas or an audio track ---------------------------------------------
            //
            // These are what the project has rather than what the operator arranged, so they
            // are edited where they are owned -- the canvas manager and the mixer -- and said
            // here rather than given a second set of fields that could disagree.
            Column {
                width: parent.width
                spacing: 6
                visible: panel.selectedKind === "canvas" || panel.selectedKind === "bus"

                Text {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: panel.selectedKind === "canvas"
                          ? qsTr("Canvases are sources on this graph. Size, rate and colour "
                               + "space are set in the canvas manager.")
                          : qsTr("Audio tracks are sources on this graph. Levels and routing "
                               + "are set in the mixer.")
                    font.pixelSize: 10
                    color: Theme.c.textMuted
                }
            }
        }
    }
}
