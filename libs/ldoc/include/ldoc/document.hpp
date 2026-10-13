// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/node.hpp"
#include "ldoc/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ldoc {

/// Bumped only by a change that older builds cannot read. Migrations are forward-only, with
/// golden fixtures; a project newer than this opens read-only rather than silently dropping the
/// fields this build does not know about.
inline constexpr int kSchemaVersion = 1;

/// An output surface with its own size, rate and colour space.
///
/// ADR-0020: a canvas is a pipeline, not a scale factor. Two canvases means two composites and
/// two independent frame deadlines, which is why they are first-class here rather than a
/// rescale applied late.
struct Canvas {
    Id id;
    std::string name;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    Fps fps{};
    ColorSpace color_space = ColorSpace::Rec709;

    /// What this canvas broadcasts when the show cannot be supervised (ADR-0025).
    ///
    /// Per canvas rather than shared, because a per-canvas override can reposition a card but
    /// cannot swap the source binding -- and a vertical show wants a separately authored vertical
    /// clip, not a letterboxed landscape one. Absent means the canvas falls back to black and
    /// silence, which is safe but is not something anyone chose.
    std::optional<Id> emergency_scene;

    friend bool operator==(const Canvas&, const Canvas&) = default;
};

enum class BusKind : std::uint8_t {
    Input,
    Submix,
    Monitor,
    /// A numbered track in the recording container.
    Track,
    /// What goes to the live outputs.
    Program,
    /// The copyright-safe recording track.
    Vod,
};

[[nodiscard]] std::string_view to_string(BusKind k);
[[nodiscard]] std::optional<BusKind> bus_kind_from_string(std::string_view s);

/// An audio bus.
///
/// Note what is *not* here: a fixed six-track checkbox grid. "Eight tracks, thirty-two inputs"
/// is a count limit on this list, not a feature, and the copyright-safe VOD track is an ordinary
/// Track bus with the music send left off -- no special case, no separate code path that can
/// disagree with the normal one.
struct Bus {
    Id id;
    std::string name;
    BusKind kind = BusKind::Submix;
    std::int32_t channels = 2;
    std::vector<Effect> inserts;
    std::vector<Send> sends;
    double gain_db = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    double delay_ms = 0.0;

    friend bool operator==(const Bus&, const Bus&) = default;
};

/// Per-scene audio routing for one layer, as a set of deltas.
struct AudioOverride {
    std::optional<Id> bus;
    std::optional<double> gain_db;
    std::optional<bool> mute;
    std::optional<std::vector<Send>> sends;

    [[nodiscard]] bool empty() const { return !bus && !gain_db && !mute && !sends; }
    friend bool operator==(const AudioOverride&, const AudioOverride&) = default;
};

/// A scene is a tree, not a list, and it is canvas-agnostic: what makes it look right on a
/// portrait canvas lives in each node's `canvas_overrides`, not in a second copy of the scene.
struct Scene {
    Id id;
    std::string name;
    /// `roots[0]` is the topmost layer. See GroupData::children.
    std::vector<Node> roots;
    /// Keyed by the layer node's id.
    std::map<Id, AudioOverride> audio_overrides;
    /// Marked by the operator as one they reach for. Part of the document rather than of the
    /// workspace, because it is a property of the show -- it should follow the project to
    /// another machine, and it is the same set whichever layout is on screen.
    bool favourite = false;

    friend bool operator==(const Scene&, const Scene&) = default;
};

/// A program/preview pair with its transition state, targeting one canvas. N canvases means N
/// decks and therefore N independent program paths.
struct Deck {
    Id id;
    std::string name;
    Id canvas;
    std::optional<Id> program_scene;
    std::optional<Id> preview_scene;
    bool studio_mode = false;
    std::string transition = "transition.cut";
    std::int32_t transition_ms = 300;

    friend bool operator==(const Deck&, const Deck&) = default;
};

/// An instance of something that produces pixels or audio.
///
/// Project-level rather than per-layer, so two layers showing the same camera open one device.
/// The `type` resolves against a manifest, like an effect does.
struct Source {
    Id id;
    std::string name;
    std::string type;
    ParamMap settings;
    /// For file-backed sources.
    std::optional<Id> asset;

    friend bool operator==(const Source&, const Source&) = default;
};

/// A file the project depends on, content-addressed so a move can be detected and repaired.
struct Asset {
    Id id;
    /// Relative to the project directory when the file lives inside it.
    std::string path;
    std::string sha256;
    std::uint64_t bytes = 0;
    /// Other places this file has been seen, for relocating it after a move.
    std::vector<std::string> relocation_hints;

    friend bool operator==(const Asset&, const Asset&) = default;
};

struct EncoderConfig {
    /// "h264", "hevc", "av1".
    std::string codec = "h264";
    /// Which implementation: "nvenc", "qsv", "vaapi", "x264", "svt-av1".
    std::string implementation = "x264";
    std::int32_t bitrate_kbps = 6000;
    std::int32_t keyframe_interval_ms = 2000;
    std::string preset;
    std::string rate_control = "cbr";
    /// Escape hatch for implementation-specific knobs.
    ParamMap extra;

    friend bool operator==(const EncoderConfig&, const EncoderConfig&) = default;
};

enum class DestinationKind : std::uint8_t { Rtmp, Rtmps, Srt, Whip, File };

[[nodiscard]] std::string_view to_string(DestinationKind k);
[[nodiscard]] std::optional<DestinationKind> destination_kind_from_string(std::string_view s);

/// Where an output goes.
///
/// **There is deliberately no field here that can hold a stream key.** `credential_ref` names an
/// entry in a secret store kept outside the project directory; the project file holds the name,
/// never the value.
///
/// This is not caution for its own sake. The project format is a directory of JSON chosen
/// *because* it is git-diffable and easy to share when asking for help -- and a stream key in
/// that file means every one of those acts leaks it. The author of this project has already had
/// a live key read out of a config dump during development. A format that cannot carry the
/// secret cannot leak it, and no amount of redaction discipline is as reliable as not having the
/// field.
struct Destination {
    DestinationKind kind = DestinationKind::Rtmp;
    /// The ingest URL. Servers sometimes put the key in the path; see `credential_ref`.
    std::string url;
    /// The name of a credential, never the credential.
    std::string credential_ref;

    friend bool operator==(const Destination&, const Destination&) = default;
};

/// One encode of one canvas going to one place.
///
/// N outputs across M canvases falls out of this being a list rather than a mode. Two outputs off
/// the same canvas share its composite; two canvases do not (ADR-0020).
struct Output {
    Id id;
    std::string name;
    Id canvas;
    EncoderConfig encoder;
    Destination destination;
    bool enabled = false;

    friend bool operator==(const Output&, const Output&) = default;
};

struct Settings {
    /// Sample rate for the whole audio graph. One rate, resampled at the edges.
    std::int32_t sample_rate = 48000;
    /// How much the renderer may run ahead, in frames.
    std::int32_t render_queue_frames = 3;
    ParamMap extra;

    friend bool operator==(const Settings&, const Settings&) = default;
};

struct Project {
    int schema_version = kSchemaVersion;
    Id id;
    std::string name;
    /// Incremented on every committed save. The save protocol writes this last, so it can never
    /// name files that are not already on disk.
    std::uint64_t generation = 0;

    std::vector<Canvas> canvases;
    std::vector<Bus> buses;
    std::vector<Scene> scenes;
    std::vector<Deck> decks;
    std::vector<Source> sources;
    std::vector<Asset> assets;
    std::vector<Output> outputs;
    Settings settings;

    [[nodiscard]] const Canvas* canvas(const Id& wanted) const;
    [[nodiscard]] const Scene* scene(const Id& wanted) const;
    [[nodiscard]] Scene* scene(const Id& wanted);
    [[nodiscard]] const Bus* bus(const Id& wanted) const;
    [[nodiscard]] const Source* source(const Id& wanted) const;

    friend bool operator==(const Project&, const Project&) = default;
};

/// A new project with one 1080p60 canvas, a program bus and a VOD track, one empty scene and a
/// deck pointed at it. Enough to open and look at; nothing that pretends to be configured.
[[nodiscard]] Project make_default_project(std::string name);

/// Duplicate a scene, minting fresh ids throughout (see `duplicate` in node.hpp for exactly which
/// references are rewritten and which are preserved).
[[nodiscard]] Scene duplicate_scene(const Scene& scene, std::string new_name);

// ---- integrity ------------------------------------------------------------------------------

enum class Severity : std::uint8_t { Error, Warning };

struct Problem {
    Severity severity = Severity::Error;
    std::string message;
    /// What the problem is about, so the UI can select it rather than just describe it.
    Id subject;
};

/// Every structural problem in the project, rather than the first one.
///
/// A document has many independent things wrong with it after a bad import or a hand-edit, and a
/// validator that stops at the first makes fixing them a game of whack-a-mole. Errors mean the
/// project cannot be rendered as written; warnings mean it can, but something in it is useless.
[[nodiscard]] std::vector<Problem> validate(const Project& project);

/// Convenience: true when `validate` returned no Error-severity problems.
[[nodiscard]] bool is_valid(const Project& project);

/// Whether a source of this type can be trusted to keep working when something else has already
/// gone wrong: no device to be unplugged, no network, no browser engine, nothing to wedge.
///
/// Unknown types answer **false**. A safety net built from something we cannot vouch for is not
/// one, and a conservative default is the only one that stays correct as source types are added.
///
/// A built-in table until source manifests exist (ADR-0004), where this belongs beside the rest
/// of a source type's declaration.
[[nodiscard]] bool is_self_contained(std::string_view source_type);

/// Why `scene` is unsuitable as an emergency scene, or empty if it is suitable.
///
/// Reported as warnings by `validate`, and used by the designation flow to refuse and to offer
/// the loop fix before the command is ever issued. The command itself permits whatever the model
/// permits -- see ADR-0023 on why a command must not refuse a state the model allows.
[[nodiscard]] std::vector<Problem> check_emergency_scene(const Project& project,
                                                         const Id& scene);

} // namespace ldoc
