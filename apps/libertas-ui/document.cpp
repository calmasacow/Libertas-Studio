// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"

#include <format>

#include "ldoc/command.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>

namespace libertas::ui {
namespace {

std::filesystem::path g_project_dir;

QString qstr(const std::string& s) { return QString::fromStdString(s); }

std::string to_std(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string{b.constData(), static_cast<std::size_t>(b.size())};
}

QString id_str(const ldoc::Id& id) { return QString::fromStdString(id.to_string()); }

std::optional<ldoc::Id> parse_id(const QString& s) {
    return ldoc::Id::parse(to_std(s));
}

} // namespace

void Document::configure(std::filesystem::path project_dir) {
    g_project_dir = std::move(project_dir);
}

Document* Document::create(QQmlEngine*, QJSEngine*) { return new Document; }

Document::Document(QObject* parent) : QObject(parent) {
    if (g_project_dir.empty()) {
        session_ = lsession::LocalSession::in_memory(ldoc::make_default_project("Untitled"));
    } else {
        std::error_code ec;
        auto opened = std::filesystem::exists(g_project_dir / "project.json", ec)
                          ? lsession::LocalSession::open(g_project_dir)
                          : lsession::LocalSession::create(
                                ldoc::make_default_project(
                                    g_project_dir.stem().string().empty()
                                        ? "Untitled"
                                        : g_project_dir.stem().string()),
                                g_project_dir);
        if (opened) {
            session_ = std::move(*opened);
        } else {
            // Falling back to an unsaved document rather than refusing to start: the operator
            // can still work, and a shell that will not open because of one project is a shell
            // they cannot use to fix it.
            session_ = lsession::LocalSession::in_memory(ldoc::make_default_project("Untitled"));
            QMetaObject::invokeMethod(
                this,
                [this, why = qstr(opened.error())] {
                    emit notice(QStringLiteral("Could not open the project: %1").arg(why));
                },
                Qt::QueuedConnection);
        }
    }

    session_->set_notice_handler([this](ldoc::Severity, const std::string& text) {
        emit notice(qstr(text));
    });
    session_->subscribe([this](const lsession::Change&) { rebuild(); });

    // Saving is debounced rather than on every command: a rename is one keystroke at a time, and
    // an fsync per keystroke is both slow and pointless when the journal already has it durably.
    autosave_.setSingleShot(true);
    autosave_.setInterval(2000);
    connect(&autosave_, &QTimer::timeout, this, [this] {
        if (session_->dirty() && !session_->path().empty())
            save();
    });

    if (!session_->project().scenes.empty())
        selected_scene_ = id_str(session_->project().scenes.front().id);
    rebuild();
}

Document::~Document() = default;

// ---- reading --------------------------------------------------------------------------------

QString Document::projectName() const { return qstr(session_->project().name); }

QString Document::projectPath() const {
    return session_->path().empty() ? QStringLiteral("(unsaved)")
                                    : qstr(session_->path().string());
}

bool Document::dirty() const { return session_->dirty(); }
int Document::revision() const { return static_cast<int>(session_->revision()); }
bool Document::canUndo() const { return session_->can_undo(); }
bool Document::canRedo() const { return session_->can_redo(); }
QString Document::undoDescription() const { return qstr(session_->undo_description()); }
QString Document::redoDescription() const { return qstr(session_->redo_description()); }

QStringList Document::problems() const {
    QStringList out;
    for (const ldoc::Problem& p : session_->problems())
        out << QStringLiteral("%1: %2")
                   .arg(p.severity == ldoc::Severity::Error ? QStringLiteral("error")
                                                            : QStringLiteral("warning"),
                        qstr(p.message));
    return out;
}

QStringList Document::blendModes() const {
    QStringList out;
    for (int i = 0; i < 25; ++i)
        out << QString::fromUtf8(ldoc::to_string(static_cast<ldoc::BlendMode>(i)).data(),
                                 static_cast<qsizetype>(
                                     ldoc::to_string(static_cast<ldoc::BlendMode>(i)).size()));
    return out;
}

const ldoc::Scene* Document::current_scene() const {
    const auto id = parse_id(selected_scene_);
    return id ? session_->project().scene(*id) : nullptr;
}

const ldoc::Node* Document::current_layer() const {
    const ldoc::Scene* s = current_scene();
    const auto id = parse_id(selected_layer_);
    if (!s || !id)
        return nullptr;
    return ldoc::find_node(s->roots, *id);
}

const ldoc::Canvas* Document::current_canvas() const {
    const auto& canvases = session_->project().canvases;
    return canvases.empty() ? nullptr : &canvases.front();
}

QVariantMap Document::layerProperties() const {
    QVariantMap m;
    const ldoc::Node* n = current_layer();
    if (!n)
        return m;
    m["id"] = id_str(n->id);
    m["name"] = qstr(n->name);
    m["kind"] = QString::fromUtf8(ldoc::to_string(n->kind()).data(),
                                  static_cast<qsizetype>(ldoc::to_string(n->kind()).size()));
    m["visible"] = n->visible;
    m["locked"] = n->locked;
    m["positionX"] = n->transform.position.x;
    m["positionY"] = n->transform.position.y;
    m["scaleX"] = n->transform.scale.x;
    m["scaleY"] = n->transform.scale.y;
    m["rotation"] = n->transform.rotation_deg;
    m["opacity"] = n->blend.opacity;
    m["blendMode"] = QString::fromUtf8(
        ldoc::to_string(n->blend.mode).data(),
        static_cast<qsizetype>(ldoc::to_string(n->blend.mode).size()));
    const auto fit = ldoc::to_string(n->layout.fit);
    m["fit"] = QString::fromUtf8(fit.data(), static_cast<qsizetype>(fit.size()));
    m["boxWidth"] = n->layout.size.x;
    m["boxHeight"] = n->layout.size.y;
    m["cropLeft"] = n->layout.crop.left;
    m["cropTop"] = n->layout.crop.top;
    m["cropRight"] = n->layout.crop.right;
    m["cropBottom"] = n->layout.crop.bottom;
    m["alignX"] = n->layout.alignment.x;
    m["alignY"] = n->layout.alignment.y;
    m["effectCount"] = static_cast<int>(n->effects.size());

    // Audio, for the mixer's selected-layer strip. A layer with no bus is unrouted, which is a
    // different thing from muted -- so both are reported rather than collapsed into one flag.
    if (const auto* layer = std::get_if<ldoc::LayerData>(&n->data)) {
        m["hasAudio"] = true;
        // What the settings dialog is opened against. Empty when the layer is bound to nothing,
        // which is what disables the gear rather than opening an empty panel.
        m["sourceId"] = layer->source ? id_str(*layer->source) : QString{};
        m["audioBus"] = layer->audio.bus ? id_str(*layer->audio.bus) : QString{};
        m["audioGain"] = layer->audio.gain_db;
        m["audioMute"] = layer->audio.mute;
        m["audioSendCount"] = static_cast<int>(layer->audio.sends.size());
        QString bus_name;
        if (layer->audio.bus) {
            const ldoc::Project& p = session_->project();
            bus_name = QStringLiteral("(missing bus)");
            for (const ldoc::Bus& b : p.buses) {
                if (b.id == *layer->audio.bus)
                    bus_name = qstr(b.name);
            }
        }
        m["audioBusName"] = bus_name;
    } else {
        m["hasAudio"] = false;
        m["sourceId"] = QString{};
    }
    return m;
}

// ---- rebuilding the models -------------------------------------------------------------------

void Document::rebuild() {
    const ldoc::Project& p = session_->project();

    // Repair the selection *before* building anything from it.
    //
    // A selected thing can be deleted by a command or brought back by an undo, so selection is
    // validated on every change rather than trusted. Doing it afterwards left the layer list
    // built from a scene that had just been removed -- empty, while a different scene was
    // selected -- and nothing triggered another rebuild, so the panel stayed empty until the
    // operator clicked something.
    // Two directions, not one. Losing the selected scene has to move the selection, and so does
    // *gaining* a scene when there is none selected -- which happens when an undo brings back
    // everything that was deleted. Without the second case the panels stay on "no scene
    // selected" with a full scene list beside them, and nothing but a click gets out of it.
    if (!current_scene()) {
        selected_scene_ = p.scenes.empty() ? QString{} : id_str(p.scenes.front().id);
        selected_layer_.clear();
    }
    if (!selected_layer_.isEmpty() && !current_layer())
        selected_layer_.clear();

    std::vector<SceneRow> scenes;
    for (const ldoc::Scene& s : p.scenes) {
        SceneRow row;
        row.id = id_str(s.id);
        row.name = qstr(s.name);
        int count = 0;
        ldoc::visit(s.roots, [&count](const ldoc::Node&) { ++count; });
        row.layerCount = count;
        for (const ldoc::Deck& d : p.decks) {
            row.isProgram = row.isProgram || (d.program_scene && *d.program_scene == s.id);
            row.isPreview = row.isPreview || (d.preview_scene && *d.preview_scene == s.id);
        }
        for (const ldoc::Canvas& c : p.canvases)
            row.isEmergency = row.isEmergency || (c.emergency_scene && *c.emergency_scene == s.id);
        row.isFavourite = s.favourite;
        scenes.push_back(std::move(row));
    }
    std::vector<SceneRow> favourites;
    for (const SceneRow& row : scenes) {
        if (row.isFavourite)
            favourites.push_back(row);
    }
    favourites_.set_rows(std::move(favourites));
    scenes_.set_rows(std::move(scenes));

    std::vector<LayerRow> layers;
    if (const ldoc::Scene* s = current_scene()) {
        // Flattened depth-first, which is what a layers panel is: the tree with indentation.
        const std::function<void(const std::vector<ldoc::Node>&, int)> walk =
            [&](const std::vector<ldoc::Node>& nodes, int depth) {
                for (const ldoc::Node& n : nodes) {
                    LayerRow row;
                    row.id = id_str(n.id);
                    row.name = qstr(n.name);
                    const auto kind = ldoc::to_string(n.kind());
                    row.kind = QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()));
                    row.depth = depth;
                    row.visible = n.visible;
                    row.locked = n.locked;
                    row.isGroup = n.is_group();
                    row.effectCount = static_cast<int>(n.effects.size());
                    if (const auto* layer = std::get_if<ldoc::LayerData>(&n.data)) {
                        if (layer->source) {
                            // The id is carried even when the source has gone: a row that says
                            // "(missing source)" with no id would look identical to an unbound
                            // one, and they want different things done to them.
                            row.sourceId = id_str(*layer->source);
                            if (const ldoc::Source* src = p.source(*layer->source))
                                row.sourceName = qstr(src->name);
                            else
                                row.sourceName = QStringLiteral("(missing source)");
                        }
                    }
                    layers.push_back(std::move(row));
                    if (const auto* kids = n.children())
                        walk(*kids, depth + 1);
                }
            };
        walk(s->roots, 0);
    }
    layers_.set_rows(std::move(layers));

    std::vector<SourceRow> sources;
    for (const ldoc::Source& src : p.sources) {
        SourceRow row;
        row.id = id_str(src.id);
        row.name = qstr(src.name);
        row.type = qstr(src.type);
        row.selfContained = ldoc::is_self_contained(src.type);
        for (const ldoc::Scene& s : p.scenes) {
            ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                const auto* layer = std::get_if<ldoc::LayerData>(&n.data);
                if (layer && layer->source && *layer->source == src.id)
                    ++row.useCount;
            });
        }
        sources.push_back(std::move(row));
    }
    sources_.set_rows(std::move(sources));

    std::vector<BusRow> buses;
    for (const ldoc::Bus& b : p.buses) {
        BusRow row;
        row.id = id_str(b.id);
        row.name = qstr(b.name);
        const auto kind = ldoc::to_string(b.kind);
        row.kind = QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()));
        row.gainDb = b.gain_db;
        row.pan = b.pan;
        row.mute = b.mute;
        row.solo = b.solo;
        row.channels = b.channels;
        row.sendCount = static_cast<int>(b.sends.size());
        // Every scene, not just the selected one: a bus is project-level, and the question the
        // strip answers is whether anything in the show feeds it at all.
        for (const ldoc::Scene& sc : p.scenes) {
            ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
                const auto* layer = std::get_if<ldoc::LayerData>(&n.data);
                if (layer && layer->audio.bus && *layer->audio.bus == b.id)
                    ++row.feedCount;
            });
        }
        if (const ldoc::Node* sel = current_layer()) {
            const auto* layer = std::get_if<ldoc::LayerData>(&sel->data);
            row.carriesSelection = layer && layer->audio.bus && *layer->audio.bus == b.id;
        }
        buses.push_back(std::move(row));
    }
    buses_.set_rows(std::move(buses));

    // Scene and canvas names, so the deck and output rows can show what is on air rather than
    // an id. Built once rather than searched per row.
    const auto scene_name = [&p](const std::optional<ldoc::Id>& id) -> QString {
        if (!id)
            return {};
        for (const ldoc::Scene& sc : p.scenes) {
            if (sc.id == *id)
                return qstr(sc.name);
        }
        return QStringLiteral("(missing scene)");
    };
    const auto canvas_name = [&p](const ldoc::Id& id) -> QString {
        for (const ldoc::Canvas& c : p.canvases) {
            if (c.id == id)
                return qstr(c.name);
        }
        return QStringLiteral("(missing canvas)");
    };

    std::vector<DeckRow> decks;
    for (const ldoc::Deck& d : p.decks) {
        DeckRow row;
        row.id = id_str(d.id);
        row.name = qstr(d.name);
        row.canvasName = canvas_name(d.canvas);
        row.programScene = scene_name(d.program_scene);
        row.previewScene = scene_name(d.preview_scene);
        row.programSceneId = d.program_scene ? id_str(*d.program_scene) : QString{};
        row.previewSceneId = d.preview_scene ? id_str(*d.preview_scene) : QString{};
        row.studioMode = d.studio_mode;
        row.transition = qstr(d.transition);
        row.transitionMs = d.transition_ms;
        for (const ldoc::Canvas& c : p.canvases) {
            if (c.id == d.canvas)
                row.hasEmergencyScene = c.emergency_scene.has_value();
        }
        decks.push_back(std::move(row));
    }
    decks_.set_rows(std::move(decks));

    std::vector<OutputRow> outputs;
    for (const ldoc::Output& o : p.outputs) {
        OutputRow row;
        row.id = id_str(o.id);
        row.name = qstr(o.name);
        row.canvasName = canvas_name(o.canvas);
        row.codec = qstr(o.encoder.codec);
        row.implementation = qstr(o.encoder.implementation);
        row.bitrateKbps = o.encoder.bitrate_kbps;
        const auto kind = ldoc::to_string(o.destination.kind);
        row.destinationKind = QString::fromUtf8(kind.data(),
                                                static_cast<qsizetype>(kind.size()));
        row.url = qstr(o.destination.url);
        // The *name* of a credential. The project format has no field for the credential
        // itself, so there is nothing here to leak (ADR-0022).
        row.credentialRef = qstr(o.destination.credential_ref);
        row.enabled = o.enabled;
        row.misconfigured = o.enabled && o.destination.url.empty() &&
                            o.destination.kind != ldoc::DestinationKind::File;
        outputs.push_back(std::move(row));
    }
    outputs_.set_rows(std::move(outputs));

    emit changed();
    emit selectionChanged();
    if (!session_->path().empty())
        autosave_.start();
}

void Document::run(const ldoc::Command& command) {
    const auto r = session_->run(command);
    if (!r)
        emit notice(qstr(r.error()));
    // No rebuild here: the session's notification does it. Updating from the return value is
    // what ADR-0024 forbids, because a remote session may return before the change has landed.
}

// ---- selection --------------------------------------------------------------------------------

void Document::selectScene(const QString& id) {
    if (selected_scene_ == id)
        return;
    selected_scene_ = id;
    selected_layer_.clear();
    rebuild();
}

void Document::selectLayer(const QString& id) {
    if (selected_layer_ == id)
        return;
    selected_layer_ = id;
    emit selectionChanged();
}

// ---- scenes -----------------------------------------------------------------------------------

void Document::addScene(const QString& name) {
    ldoc::Scene s;
    s.id = ldoc::Id::generate();
    s.name = name.isEmpty() ? std::string{"Scene"} : to_std(name);
    const QString id = id_str(s.id);
    run(ldoc::AddScene{std::move(s), session_->project().scenes.size()});
    selectScene(id);
}

void Document::removeScene(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveScene{*parsed});
}

void Document::renameScene(const QString& id, const QString& name) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RenameScene{*parsed, to_std(name)});
}

void Document::duplicateScene(const QString& id) {
    const auto parsed = parse_id(id);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    if (!s)
        return;
    ldoc::Scene copy = ldoc::duplicate_scene(*s, s->name + " copy");
    const QString new_id = id_str(copy.id);
    run(ldoc::AddScene{std::move(copy), session_->project().scenes.size()});
    selectScene(new_id);
}

void Document::setSceneFavourite(const QString& id, bool favourite) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::SetSceneFavourite{*parsed, favourite});
}

void Document::moveScene(const QString& id, int delta) {
    const auto parsed = parse_id(id);
    if (!parsed || delta == 0)
        return;
    const auto& scenes = session_->project().scenes;
    const auto it = std::find_if(scenes.begin(), scenes.end(),
                                 [&parsed](const ldoc::Scene& s) { return s.id == *parsed; });
    if (it == scenes.end())
        return;
    const auto from = static_cast<std::ptrdiff_t>(it - scenes.begin());
    const auto to = from + delta;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(scenes.size()))
        return; // already at the end, which is not an error
    run(ldoc::MoveScene{*parsed, static_cast<std::size_t>(to)});
}

QStringList Document::emergencySceneProblems(const QString& id) const {
    QStringList out;
    const auto parsed = parse_id(id);
    if (!parsed)
        return out;
    for (const ldoc::Problem& p : ldoc::check_emergency_scene(session_->project(), *parsed))
        out << qstr(p.message);
    return out;
}

void Document::setEmergencyScene(const QString& id) {
    const auto parsed = parse_id(id);
    const ldoc::Canvas* canvas = current_canvas();
    if (!parsed || !canvas)
        return;
    // The command would accept this regardless -- ADR-0023 -- so the check belongs here, in the
    // designation flow, where there is a human to tell.
    for (const ldoc::Problem& p : ldoc::check_emergency_scene(session_->project(), *parsed))
        emit notice(qstr(p.message));
    run(ldoc::SetCanvasEmergencyScene{canvas->id, *parsed});
}

// ---- inputs -------------------------------------------------------------------------------

namespace {

/// sRGB hex, which is what a human types and reads. The conversion to scene-linear happens in
/// `Rgba` and nowhere else (ldoc/types.hpp), so this stays a pure spelling change.
QString hex_of(const ldoc::Rgba& rgba) {
    const auto c = rgba.to_srgb8();
    return QStringLiteral("#%1%2%3")
        .arg(c[0], 2, 16, QLatin1Char('0'))
        .arg(c[1], 2, 16, QLatin1Char('0'))
        .arg(c[2], 2, 16, QLatin1Char('0'));
}

/// The other direction. Returns nothing for anything it cannot read, rather than black: a typo
/// in a hex field silently turning a card black is the kind of thing nobody notices until the
/// show.
std::optional<ldoc::Rgba> hex_to_rgba(const QString& text) {
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);
    if (hex.size() != 6 && hex.size() != 8)
        return std::nullopt;
    bool ok = false;
    const uint packed = hex.toUInt(&ok, 16);
    if (!ok)
        return std::nullopt;
    if (hex.size() == 6) {
        return ldoc::Rgba::from_srgb8(static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
                                      static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
                                      static_cast<std::uint8_t>(packed & 0xFFu));
    }
    return ldoc::Rgba::from_srgb8(static_cast<std::uint8_t>((packed >> 24) & 0xFFu),
                                  static_cast<std::uint8_t>((packed >> 16) & 0xFFu),
                                  static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
                                  static_cast<std::uint8_t>(packed & 0xFFu));
}

QString swatch_of(const ldoc::Source& src) {
    const auto it = src.settings.find("color");
    if (it == src.settings.end())
        return {};
    const auto* rgba = std::get_if<ldoc::Rgba>(&it->second);
    return rgba ? hex_of(*rgba) : QString{};
}

/// One row of a source type's settings panel.
///
/// A table rather than a class per type. Ten of these are a description of what a source *is*,
/// and a description is easier to read, review and extend as data than as code -- the render
/// process will eventually be handed exactly these keys, and a table is what can be compared
/// against what it accepts.
struct FieldSpec {
    /// The key in `Source::settings`. Empty for a note.
    const char* key;
    /// What the field is called -- or, for a note, the whole line of prose.
    const char* label;
    /// "text", "path", "bool", "int", "choice", "colour", or "note".
    const char* kind;
    /// The default for text, path, choice and colour.
    const char* text = "";
    bool flag = false;
    std::int64_t number = 0;
    /// A "choice" field's options, separated by '|'.
    const char* choices = "";
    /// Printed after an "int" field: a unit, not a value.
    const char* suffix = "";
};

// A note is honest about what this build cannot do, placed above the field it is about rather
// than collected at the top where nobody reads it.

constexpr FieldSpec kMedia[] = {
    {"localFile", "File", "path", "", false, 0, "", ""},
    // Looping by default is not a preference. `check_emergency_scene` reads this key by name
    // and warns when it is off, and a clip that plays once is a safety net that expires.
    {"loop", "Loop", "bool", "", true},
    {"", "A media source in an emergency scene has to loop, or the safety net runs out part "
         "way through the emergency (ADR-0025).", "note"},
    {"restartOnActivate", "Restart playback when the source becomes active", "bool", "", true},
    {"clearOnEnd", "Show nothing when playback ends", "bool", "", true},
    {"closeWhenInactive", "Close the file when the source is not showing", "bool", "", false},
    {"hardwareDecode", "Use hardware decoding when available", "bool", "", false},
    {"speedPercent", "Speed", "int", "", false, 100, "", "%"},
};

constexpr FieldSpec kImage[] = {
    {"file", "Image file", "path"},
    {"unloadWhenHidden", "Unload the image when it is not showing", "bool", "", false},
    {"alphaLinear", "Apply alpha in linear space", "bool", "", false},
};

constexpr FieldSpec kText[] = {
    {"text", "Text", "text"},
    {"font", "Font", "text", "Sans"},
    {"size", "Size", "int", "", false, 48, "", "px"},
    {"color", "Colour", "colour", "#ffffff"},
    {"alignment", "Alignment", "choice", "Left", false, 0, "Left|Centre|Right"},
};

constexpr FieldSpec kColour[] = {
    {"color", "Colour", "colour", "#3b82f6"},
    {"width", "Width", "int", "", false, 1920, "", "px"},
    {"height", "Height", "int", "", false, 1080, "", "px"},
};

// Which screen, and which window, are chosen by the portal picker at capture time and cannot be
// chosen here: a Wayland client is not told what is on the machine and may not pick for itself
// (ADR-0003). That is a property of the platform, not a gap in this build, so it is stated as
// such rather than left as an empty dropdown.
constexpr FieldSpec kScreen[] = {
    {"", "Which screen is chosen in the portal's own picker when capture starts. A Wayland "
         "client cannot enumerate or choose one for itself (ADR-0003).", "note"},
    {"showCursor", "Show the cursor", "bool", "", true},
    {"pauseWhenHidden", "Stop capturing while the source is not on screen", "bool", "", true},
};

constexpr FieldSpec kWindow[] = {
    {"", "Which window is chosen in the portal's own picker when capture starts. A Wayland "
         "client cannot enumerate or choose one for itself (ADR-0003).", "note"},
    {"showCursor", "Show the cursor", "bool", "", true},
    {"pauseWhenHidden", "Stop capturing while the source is not on screen", "bool", "", true},
};

constexpr FieldSpec kCamera[] = {
    {"", "This build enumerates no devices, so the node is typed rather than picked from a "
         "list. `libertas probe` reports what is attached to this machine.", "note"},
    {"device", "Device", "text", "/dev/video0"},
    {"resolution", "Resolution", "choice", "Leave unchanged", false, 0,
     "Leave unchanged|1920x1080|1280x720|960x540|640x480"},
    {"frameRate", "Frame rate", "choice", "Leave unchanged", false, 0,
     "Leave unchanged|60|59.94|50|30|29.97|25|24"},
    {"colourRange", "Colour range", "choice", "Default", false, 0, "Default|Partial|Full"},
    {"buffering", "Use buffering", "bool", "", true},
    {"autoresetOnTimeout", "Reset the device when it stops sending", "bool", "", false},
    {"framesUntilTimeout", "Frames until timeout", "int", "", false, 5},
};

constexpr FieldSpec kBrowser[] = {
    {"url", "URL", "text", "https://"},
    {"", "A browser engine runs as a worker process of its own and is never loaded into ours "
         "(ADR-0004), so a page that wedges costs this source and nothing else.", "note"},
    {"width", "Width", "int", "", false, 1920, "", "px"},
    {"height", "Height", "int", "", false, 1080, "", "px"},
    {"fps", "Frame rate", "int", "", false, 30, "", "fps"},
    {"css", "Custom CSS", "text"},
    {"shutdownWhenHidden", "Shut the page down when it is not showing", "bool", "", false},
    {"refreshOnActivate", "Reload when the source becomes active", "bool", "", false},
};

constexpr FieldSpec kAudioInput[] = {
    {"", "This build enumerates no devices, so the node is typed rather than picked from a "
         "list. `libertas probe` reports what PipeWire offers.", "note"},
    {"device", "Device", "text", "default"},
    {"channels", "Channels", "choice", "Stereo", false, 0, "Mono|Stereo"},
    {"useDeviceTiming", "Timestamp from the device rather than the clock", "bool", "", true},
};

constexpr FieldSpec kAudioOutput[] = {
    {"", "This build enumerates no devices, so the node is typed rather than picked from a "
         "list. `libertas probe` reports what PipeWire offers.", "note"},
    {"device", "Device", "text", "default"},
    {"channels", "Channels", "choice", "Stereo", false, 0, "Mono|Stereo"},
    {"useDeviceTiming", "Timestamp from the device rather than the clock", "bool", "", true},
};

std::span<const FieldSpec> schema_for(std::string_view type) {
    if (type == "source.media")        return kMedia;
    if (type == "source.image")        return kImage;
    if (type == "source.text")         return kText;
    if (type == "source.color")        return kColour;
    if (type == "source.screen")       return kScreen;
    if (type == "source.window")       return kWindow;
    if (type == "source.v4l2")         return kCamera;
    if (type == "source.browser")      return kBrowser;
    if (type == "source.audio-input")  return kAudioInput;
    if (type == "source.audio-output") return kAudioOutput;
    // A type this build does not know -- a project from a later one. No fields rather than a
    // guess at them, and the dialog says so on its face instead of offering an empty grid.
    return {};
}

bool is_note(const FieldSpec& f) { return std::string_view{f.kind} == "note"; }

QVariant default_of(const FieldSpec& f) {
    const std::string_view kind{f.kind};
    if (kind == "bool")
        return f.flag;
    if (kind == "int")
        return QVariant::fromValue(static_cast<qlonglong>(f.number));
    return QString::fromUtf8(f.text);
}

/// A stored value read back as the kind the schema declares, or invalid when it is stored as
/// something else entirely -- a hand-edited project, or a key a later build meant differently.
QVariant stored_as(const ldoc::ParamValue& v, std::string_view kind) {
    if (kind == "bool") {
        const auto* b = std::get_if<bool>(&v);
        return b ? QVariant{*b} : QVariant{};
    }
    if (kind == "int") {
        if (const auto* i = std::get_if<std::int64_t>(&v))
            return QVariant::fromValue(static_cast<qlonglong>(*i));
        if (const auto* d = std::get_if<double>(&v))
            return QVariant::fromValue(static_cast<qlonglong>(std::llround(*d)));
        return {};
    }
    if (kind == "colour") {
        const auto* c = std::get_if<ldoc::Rgba>(&v);
        return c ? QVariant{hex_of(*c)} : QVariant{};
    }
    const auto* s = std::get_if<std::string>(&v);
    return s ? QVariant{qstr(*s)} : QVariant{};
}

/// What a brand-new source of this type is: every field the schema names, written out.
///
/// Not left sparse. The project format stores defaults rather than omitting them (ldoc's
/// `ParamValue`), precisely so that a later build changing its mind about a default cannot
/// silently change what an existing project means. A source created with an empty settings map
/// is the sparse case arriving by the back door.
ldoc::ParamMap default_settings(std::string_view type) {
    ldoc::ParamMap out;
    for (const FieldSpec& f : schema_for(type)) {
        if (is_note(f))
            continue;
        const std::string_view kind{f.kind};
        if (kind == "bool")
            out.emplace(f.key, f.flag);
        else if (kind == "int")
            out.emplace(f.key, f.number);
        else if (kind == "colour")
            out.emplace(f.key, hex_to_rgba(QString::fromLatin1(f.text)).value_or(ldoc::Rgba{}));
        else
            out.emplace(f.key, std::string{f.text});
    }
    return out;
}

} // namespace


QVariantList Document::sourceTypes() const {
    struct Entry {
        const char* id;
        const char* title;
        /// One glyph, drawn where OBS draws an icon. Chosen from what DejaVu and Noto both
        /// carry, so a missing font does not leave a row of empty boxes.
        const char* icon;
    };
    // Ordered so the ones that are safe in an emergency scene come first, since that is the
    // list an operator is reading when it matters most (ADR-0025).
    static constexpr Entry kTypes[] = {
        {"source.media", "Media File", "\u25B6"},
        {"source.image", "Image", "\u25A8"},
        {"source.text", "Text", "T"},
        {"source.color", "Colour", "\u25A0"},
        {"source.screen", "Screen", "\u25AD"},
        {"source.window", "Window", "\u2750"},
        {"source.v4l2", "Camera", "\u25C9"},
        {"source.browser", "Browser", "\u25CD"},
        {"source.audio-input", "Audio Input", "\u266A"},
        {"source.audio-output", "Audio Output", "\u266B"},
    };
    QVariantList out;
    for (const Entry& e : kTypes) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(e.id);
        m["title"] = QString::fromLatin1(e.title);
        m["icon"] = QString::fromUtf8(e.icon);
        m["selfContained"] = ldoc::is_self_contained(e.id);
        out.append(m);
    }
    return out;
}

namespace {

/// Where a new layer goes: inside the selected group if one is selected, otherwise at the top.
struct Placement {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

} // namespace

void Document::addSourceAsLayer(const QString& name, const QString& type, bool visible) {
    const ldoc::Scene* scene = current_scene();
    if (!scene) {
        emit notice(QStringLiteral("There is no scene to add a source to."));
        return;
    }
    if (type.isEmpty())
        return;

    const ldoc::Project& p = session_->project();

    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = name.isEmpty() ? std::string{"Source"} : to_std(name);
    src.type = to_std(type);
    // Fully configured from the moment it exists, from the same table the gear's panel draws
    // from. That is what gives a colour its real swatch for the add dialog to show, and what
    // makes a media source loop -- which the emergency scene needs (ADR-0025) and which the
    // check for one looks for by name.
    src.settings = default_settings(src.type);
    const ldoc::Id source_id = src.id;

    ldoc::Node layer = ldoc::make_node(ldoc::NodeKind::Layer, src.name);
    layer.visible = visible;
    const ldoc::Id layer_id = layer.id;

    Placement where;
    if (const ldoc::Node* sel = current_layer(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddSource{std::move(src), p.sources.size()},
                          ldoc::AddNode{scene->id, where.parent, where.index, std::move(layer)},
                          ldoc::SetLayerSource{scene->id, layer_id, source_id}},
                         std::format("Add {}", name.isEmpty() ? std::string{"source"}
                                                              : to_std(name))));
    selectLayer(id_str(layer_id));
}

void Document::addExistingSourceAsLayer(const QString& sourceId, bool visible) {
    const ldoc::Scene* scene = current_scene();
    const auto parsed = parse_id(sourceId);
    if (!scene || !parsed)
        return;
    const ldoc::Source* existing = session_->project().source(*parsed);
    if (!existing) {
        emit notice(QStringLiteral("That source is no longer in the project."));
        return;
    }

    ldoc::Node layer = ldoc::make_node(ldoc::NodeKind::Layer, existing->name);
    layer.visible = visible;
    const ldoc::Id layer_id = layer.id;

    Placement where;
    if (const ldoc::Node* sel = current_layer(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddNode{scene->id, where.parent, where.index, std::move(layer)},
                          ldoc::SetLayerSource{scene->id, layer_id, *parsed}},
                         std::format("Add {}", existing->name)));
    selectLayer(id_str(layer_id));
}


QVariantMap Document::source_tile(const ldoc::Source& src) const {
    const ldoc::Project& p = session_->project();
    QVariantMap m;
    m["id"] = id_str(src.id);
    m["name"] = qstr(src.name);
    m["type"] = qstr(src.type);
    m["selfContained"] = ldoc::is_self_contained(src.type);
    m["swatch"] = swatch_of(src);

    QString icon;
    QString type_title = qstr(src.type);
    for (const QVariant& v : sourceTypes()) {
        const QVariantMap entry = v.toMap();
        if (entry.value("id").toString() == qstr(src.type)) {
            icon = entry.value("icon").toString();
            type_title = entry.value("title").toString();
        }
    }
    m["icon"] = icon;
    m["typeTitle"] = type_title;

    int uses = 0;
    for (const ldoc::Scene& sc : p.scenes) {
        ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
            const auto* layer = std::get_if<ldoc::LayerData>(&n.data);
            if (layer && layer->source && *layer->source == src.id)
                ++uses;
        });
    }
    m["useCount"] = uses;
    return m;
}

QVariantList Document::sourcesOfType(const QString& type) const {
    QVariantList out;
    for (const ldoc::Source& src : session_->project().sources) {
        if (qstr(src.type) == type)
            out.append(source_tile(src));
    }
    return out;
}

QVariantList Document::recentSources(int limit) const {
    const auto& sources = session_->project().sources;
    QVariantList out;
    // Newest first. Sources are appended, so the tail reversed is the real creation order
    // rather than a guess at one.
    for (auto it = sources.rbegin(); it != sources.rend(); ++it) {
        if (limit > 0 && out.size() >= limit)
            break;
        out.append(source_tile(*it));
    }
    return out;
}

// ---- a source's own settings ------------------------------------------------------------


QVariantList Document::sourceSettingsSchema(const QString& type) const {
    QVariantList out;
    for (const FieldSpec& f : schema_for(to_std(type))) {
        QVariantMap m;
        m["kind"] = QString::fromLatin1(f.kind);
        m["key"] = QString::fromLatin1(f.key);
        m["label"] = QString::fromUtf8(f.label);
        m["value"] = is_note(f) ? QVariant{QString{}} : default_of(f);
        m["suffix"] = QString::fromLatin1(f.suffix);
        const QString choices = QString::fromLatin1(f.choices);
        m["choices"] = choices.isEmpty() ? QStringList{} : choices.split(QLatin1Char('|'));
        out.append(m);
    }
    return out;
}

QVariantMap Document::sourceDetails(const QString& id) const {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src)
        return {};

    QVariantMap m = source_tile(*src);
    QVariantMap settings;
    for (const FieldSpec& f : schema_for(src->type)) {
        if (is_note(f))
            continue;
        QVariant v;
        if (const auto it = src->settings.find(f.key); it != src->settings.end())
            v = stored_as(it->second, f.kind);
        // Absent, or stored as something this field cannot be: either way the default shows,
        // so every field is always editable rather than blank and unexplained.
        settings[QString::fromLatin1(f.key)] = v.isValid() ? v : default_of(f);
    }
    m["settings"] = settings;
    m["fieldCount"] = static_cast<int>(settings.size());
    return m;
}

void Document::applySourceSettings(const QString& id, const QString& name,
                                   const QVariantMap& values) {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src) {
        emit notice(QStringLiteral("That source is no longer in the project."));
        return;
    }

    ldoc::Source copy = *src;
    if (!name.trimmed().isEmpty())
        copy.name = to_std(name.trimmed());

    for (const FieldSpec& f : schema_for(copy.type)) {
        if (is_note(f))
            continue;
        const auto it = values.constFind(QString::fromLatin1(f.key));
        if (it == values.constEnd())
            continue;
        const std::string_view kind{f.kind};
        if (kind == "bool") {
            copy.settings.insert_or_assign(std::string{f.key}, it->toBool());
        } else if (kind == "int") {
            copy.settings.insert_or_assign(std::string{f.key},
                                           static_cast<std::int64_t>(it->toLongLong()));
        } else if (kind == "colour") {
            // Refused rather than rounded to something: see `hex_to_rgba`.
            if (const auto rgba = hex_to_rgba(it->toString()))
                copy.settings.insert_or_assign(std::string{f.key}, *rgba);
            else
                emit notice(QStringLiteral("'%1' is not a colour, so %2 was left as it was.")
                                .arg(it->toString(), QString::fromUtf8(f.label)));
        } else {
            copy.settings.insert_or_assign(std::string{f.key}, to_std(it->toString()));
        }
    }

    // Nothing was actually changed -- the dialog was opened and accepted. An undo step for that
    // is a step the operator has to press Ctrl+Z through later to reach a real one.
    if (copy == *src)
        return;
    run(ldoc::SetSource{std::move(copy)});
}

void Document::addSource(const QString& name, const QString& type) {
    if (type.isEmpty())
        return;
    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = name.isEmpty() ? std::string{"Input"} : to_std(name);
    src.type = to_std(type);
    src.settings = default_settings(src.type);
    run(ldoc::AddSource{std::move(src), session_->project().sources.size()});
}

void Document::removeSource(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveSource{*parsed});
}

void Document::renameSource(const QString& id, const QString& name) {
    const auto parsed = parse_id(id);
    const ldoc::Source* src = parsed ? session_->project().source(*parsed) : nullptr;
    if (!src)
        return;
    ldoc::Source copy = *src;
    copy.name = to_std(name);
    run(ldoc::SetSource{std::move(copy)});
}

void Document::bindSelectedLayer(const QString& sourceId) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    if (!std::holds_alternative<ldoc::LayerData>(n->data)) {
        emit notice(QStringLiteral("Only a layer can be bound to an input."));
        return;
    }
    run(ldoc::SetLayerSource{s->id, n->id,
                             sourceId.isEmpty() ? std::nullopt : parse_id(sourceId)});
}

// ---- buses ------------------------------------------------------------------------------------

QVariantList Document::busKinds() const {
    struct Entry {
        ldoc::BusKind kind;
        const char* title;
        const char* note;
    };
    static constexpr Entry kKinds[] = {
        {ldoc::BusKind::Submix, "Submix", "A group of inputs with one fader"},
        {ldoc::BusKind::Program, "Program", "What goes to the live outputs"},
        {ldoc::BusKind::Vod, "VOD", "The recording track with the music left off"},
        {ldoc::BusKind::Track, "Track", "A numbered track in the recording file"},
        {ldoc::BusKind::Monitor, "Monitor", "What you hear, which nobody else does"},
        {ldoc::BusKind::Input, "Input", "One device, before anything is done to it"},
    };
    QVariantList out;
    for (const Entry& e : kKinds) {
        const auto id = ldoc::to_string(e.kind);
        QVariantMap m;
        m["id"] = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        m["title"] = QString::fromLatin1(e.title);
        m["note"] = QString::fromLatin1(e.note);
        out.append(m);
    }
    return out;
}

void Document::addBus(const QString& name, const QString& kind) {
    const auto parsed = ldoc::bus_kind_from_string(to_std(kind));
    if (!parsed) {
        emit notice(QStringLiteral("'%1' is not a kind of bus this build knows.").arg(kind));
        return;
    }
    ldoc::Bus b;
    b.id = ldoc::Id::generate();
    b.name = name.isEmpty() ? std::string{"Bus"} : to_std(name);
    b.kind = *parsed;
    run(ldoc::AddBus{std::move(b), session_->project().buses.size()});
}

void Document::removeBus(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveBus{*parsed});
}

/// Read-modify-write on one bus. The command carries the whole bus rather than a delta, so each
/// of these has to start from the current one -- and there is exactly one place that does it,
/// because a copy of this that forgot a field would silently reset it.
void Document::edit_bus(const QString& id, const std::function<void(ldoc::Bus&)>& change,
                        std::string_view gesture) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Bus* existing = nullptr;
    for (const ldoc::Bus& b : session_->project().buses) {
        if (b.id == *parsed)
            existing = &b;
    }
    if (!existing)
        return;
    ldoc::Bus next = *existing;
    change(next);
    if (next == *existing)
        return; // nothing to put on the undo stack
    if (gesture.empty()) {
        run(ldoc::SetBus{std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetBus{std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::renameBus(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_bus(id, [&](ldoc::Bus& b) { b.name = to_std(name); });
}

void Document::setBusGain(const QString& id, double gainDb, bool dragging) {
    // One gesture per fader, named by the bus, so dragging two of them in turn is two undo
    // steps even if the operator never lets go in between.
    const std::string gesture =
        dragging ? std::format("bus.gain:{}", to_std(id)) : std::string{};
    edit_bus(id, [gainDb](ldoc::Bus& b) { b.gain_db = gainDb; }, gesture);
}

void Document::endGesture() {
    session_->end_gesture();
}

void Document::setBusPan(const QString& id, double pan) {
    edit_bus(id, [pan](ldoc::Bus& b) { b.pan = std::clamp(pan, -1.0, 1.0); });
}

void Document::setBusMute(const QString& id, bool mute) {
    edit_bus(id, [mute](ldoc::Bus& b) { b.mute = mute; });
}

void Document::setBusSolo(const QString& id, bool solo) {
    edit_bus(id, [solo](ldoc::Bus& b) { b.solo = solo; });
}

/// Same shape for the selected layer's own routing, for the same reason.
void Document::edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    const auto* layer = std::get_if<ldoc::LayerData>(&n->data);
    if (!layer) {
        emit notice(QStringLiteral("Only a layer has audio of its own."));
        return;
    }
    ldoc::AudioRouting next = layer->audio;
    change(next);
    if (next == layer->audio)
        return;
    run(ldoc::SetNodeAudio{s->id, n->id, std::move(next)});
}

void Document::routeSelectedLayer(const QString& busId) {
    edit_selected_audio([&](ldoc::AudioRouting& a) {
        a.bus = busId.isEmpty() ? std::nullopt : parse_id(busId);
    });
}

void Document::setSelectedLayerGain(double gainDb) {
    edit_selected_audio([gainDb](ldoc::AudioRouting& a) { a.gain_db = gainDb; });
}

void Document::setSelectedLayerMute(bool mute) {
    edit_selected_audio([mute](ldoc::AudioRouting& a) { a.mute = mute; });
}

// ---- decks and outputs ------------------------------------------------------------------------

QVariantList Document::transitions() const {
    struct Entry {
        const char* id;
        const char* title;
    };
    static constexpr Entry kTransitions[] = {
        {"transition.cut", "Cut"},
        {"transition.fade", "Fade"},
        {"transition.fade-to-black", "Fade to Black"},
        {"transition.slide", "Slide"},
    };
    QVariantList out;
    for (const Entry& e : kTransitions) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(e.id);
        m["title"] = QString::fromLatin1(e.title);
        // A cut needs nothing from a renderer; the rest do, and the panel marks them so an
        // operator is not told a fade works when nothing can draw one yet.
        m["implemented"] = std::string_view{e.id} == "transition.cut";
        out.append(m);
    }
    return out;
}

QVariantList Document::destinationKinds() const {
    static constexpr ldoc::DestinationKind kKinds[] = {
        ldoc::DestinationKind::Rtmps, ldoc::DestinationKind::Rtmp,
        ldoc::DestinationKind::Srt,   ldoc::DestinationKind::Whip,
        ldoc::DestinationKind::File,
    };
    QVariantList out;
    for (const ldoc::DestinationKind k : kKinds) {
        const auto id = ldoc::to_string(k);
        QVariantMap m;
        m["id"] = QString::fromUtf8(id.data(), static_cast<qsizetype>(id.size()));
        m["title"] = m["id"].toString().toUpper();
        out.append(m);
    }
    return out;
}

QVariantList Document::codecs() const {
    // Deliberately not a capability list. What a machine can actually encode is `libertas probe`'s
    // question and is answered at run time per device; this is only what the document may say.
    struct Entry {
        const char* codec;
        const char* impl;
        const char* title;
    };
    static constexpr Entry kCodecs[] = {
        {"h264", "nvenc", "H.264 (NVENC)"},    {"h264", "qsv", "H.264 (QuickSync)"},
        {"h264", "vaapi", "H.264 (VAAPI)"},    {"h264", "x264", "H.264 (x264)"},
        {"hevc", "nvenc", "HEVC (NVENC)"},     {"hevc", "qsv", "HEVC (QuickSync)"},
        {"av1", "nvenc", "AV1 (NVENC)"},       {"av1", "qsv", "AV1 (QuickSync)"},
        {"av1", "svt-av1", "AV1 (SVT)"},
    };
    QVariantList out;
    for (const Entry& e : kCodecs) {
        QVariantMap m;
        m["codec"] = QString::fromLatin1(e.codec);
        m["implementation"] = QString::fromLatin1(e.impl);
        m["title"] = QString::fromLatin1(e.title);
        out.append(m);
    }
    return out;
}

void Document::edit_deck(const QString& id, const std::function<void(ldoc::Deck&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Deck* existing = nullptr;
    for (const ldoc::Deck& d : session_->project().decks) {
        if (d.id == *parsed)
            existing = &d;
    }
    if (!existing)
        return;
    ldoc::Deck next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetDeck{std::move(next)});
}

void Document::setDeckProgram(const QString& deckId, const QString& sceneId) {
    const auto deck = parse_id(deckId);
    if (!deck)
        return;
    run(ldoc::SetDeckProgram{*deck, sceneId.isEmpty() ? std::nullopt : parse_id(sceneId)});
}

void Document::setDeckPreview(const QString& deckId, const QString& sceneId) {
    const auto deck = parse_id(deckId);
    if (!deck)
        return;
    run(ldoc::SetDeckPreview{*deck, sceneId.isEmpty() ? std::nullopt : parse_id(sceneId)});
}

void Document::takeDeck(const QString& deckId) {
    const auto parsed = parse_id(deckId);
    if (!parsed)
        return;
    for (const ldoc::Deck& d : session_->project().decks) {
        if (d.id != *parsed)
            continue;
        if (!d.preview_scene) {
            emit notice(QStringLiteral("Nothing is staged on preview."));
            return;
        }
        if (d.program_scene && *d.program_scene == *d.preview_scene)
            return; // already on air; a take would be a no-op on the undo stack
        run(ldoc::SetDeckProgram{d.id, d.preview_scene});
        return;
    }
}

void Document::setDeckStudioMode(const QString& deckId, bool studio) {
    edit_deck(deckId, [studio](ldoc::Deck& d) { d.studio_mode = studio; });
}

void Document::setDeckTransition(const QString& deckId, const QString& transition,
                                 int durationMs) {
    edit_deck(deckId, [&](ldoc::Deck& d) {
        if (!transition.isEmpty())
            d.transition = to_std(transition);
        if (durationMs >= 0)
            d.transition_ms = durationMs;
    });
}

void Document::addOutput(const QString& name) {
    const ldoc::Project& p = session_->project();
    if (p.canvases.empty()) {
        emit notice(QStringLiteral("An output needs a canvas to encode."));
        return;
    }
    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = name.isEmpty() ? std::string{"Output"} : to_std(name);
    o.canvas = p.canvases.front().id;
    // Disabled, with nowhere to send to. An output that arrived enabled and pointed at a URL
    // would be one keystroke from going somewhere the operator did not choose.
    o.enabled = false;
    run(ldoc::AddOutput{std::move(o), p.outputs.size()});
}

void Document::removeOutput(const QString& id) {
    const auto parsed = parse_id(id);
    if (parsed)
        run(ldoc::RemoveOutput{*parsed});
}

void Document::edit_output(const QString& id, const std::function<void(ldoc::Output&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Output* existing = nullptr;
    for (const ldoc::Output& o : session_->project().outputs) {
        if (o.id == *parsed)
            existing = &o;
    }
    if (!existing)
        return;
    ldoc::Output next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetOutput{std::move(next)});
}

void Document::renameOutput(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_output(id, [&](ldoc::Output& o) { o.name = to_std(name); });
}

void Document::setOutputEnabled(const QString& id, bool enabled) {
    edit_output(id, [enabled](ldoc::Output& o) { o.enabled = enabled; });
}

void Document::setOutputBitrate(const QString& id, int kbps) {
    edit_output(id, [kbps](ldoc::Output& o) { o.encoder.bitrate_kbps = kbps; });
}

void Document::setOutputEncoder(const QString& id, const QString& codec,
                                const QString& implementation) {
    edit_output(id, [&](ldoc::Output& o) {
        o.encoder.codec = to_std(codec);
        o.encoder.implementation = to_std(implementation);
    });
}

void Document::setOutputDestination(const QString& id, const QString& kind, const QString& url) {
    const auto parsed_kind = ldoc::destination_kind_from_string(to_std(kind));
    if (!kind.isEmpty() && !parsed_kind) {
        emit notice(QStringLiteral("'%1' is not a kind of destination this build knows.")
                        .arg(kind));
        return;
    }
    edit_output(id, [&](ldoc::Output& o) {
        if (parsed_kind)
            o.destination.kind = *parsed_kind;
        o.destination.url = to_std(url);
    });
}

void Document::setOutputCredentialRef(const QString& id, const QString& ref) {
    edit_output(id, [&](ldoc::Output& o) { o.destination.credential_ref = to_std(ref); });
}

// ---- layers -----------------------------------------------------------------------------------

namespace {

/// Where a new node goes: inside the selected group if one is selected, otherwise at the top of
/// the scene. Top, because `roots[0]` is the front-most layer and a new thing should be visible.
struct Destination {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

} // namespace

void Document::addLayer(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Layer,
                                   name.isEmpty() ? std::string{"Layer"} : to_std(name));
    const QString id = id_str(n.id);

    Destination dest;
    if (const ldoc::Node* sel = current_layer(); sel && sel->is_group())
        dest.parent = sel->id;
    run(ldoc::AddNode{s->id, dest.parent, dest.index, std::move(n)});
    selectLayer(id);
}

void Document::addGroup(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Group,
                                   name.isEmpty() ? std::string{"Group"} : to_std(name));
    const QString id = id_str(n.id);
    run(ldoc::AddNode{s->id, std::nullopt, 0, std::move(n)});
    selectLayer(id);
}

void Document::removeLayer(const QString& id) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RemoveNode{s->id, *parsed});
}

void Document::renameLayer(const QString& id, const QString& name) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RenameNode{s->id, *parsed, to_std(name)});
}

void Document::setLayerVisible(const QString& id, bool visible) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeVisible{s->id, *parsed, visible});
}

void Document::setLayerLocked(const QString& id, bool locked) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeLocked{s->id, *parsed, locked});
}

void Document::moveLayer(const QString& id, int delta) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (!s || !parsed || delta == 0)
        return;

    // Find the node's siblings so the move stays within its own level: a layers panel's up and
    // down move a thing within its group, not out of it.
    const std::function<bool(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> search =
        [&](const std::vector<ldoc::Node>& nodes,
            std::optional<ldoc::Id> parent) -> bool {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].id == *parsed) {
                const auto target = static_cast<std::ptrdiff_t>(i) + delta;
                if (target < 0 || target >= static_cast<std::ptrdiff_t>(nodes.size()))
                    return true; // already at the end; not an error, just nothing to do
                run(ldoc::MoveNode{s->id, *parsed, parent,
                                   static_cast<std::size_t>(target)});
                return true;
            }
            if (const auto* kids = nodes[i].children()) {
                if (search(*kids, nodes[i].id))
                    return true;
            }
        }
        return false;
    };
    search(s->roots, std::nullopt);
}

// ---- properties -------------------------------------------------------------------------------

void Document::setLayerPosition(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.position = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setLayerScale(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.scale = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setLayerRotation(double degrees) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.rotation_deg = degrees;
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setLayerOpacity(double opacity) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Blend b = n->blend;
    b.opacity = std::clamp(opacity, 0.0, 1.0);
    run(ldoc::SetNodeBlend{s->id, n->id, b});
}

QStringList Document::fitModes() const {
    QStringList out;
    for (int i = 0; i < 6; ++i) {
        const auto name = ldoc::to_string(static_cast<ldoc::FitMode>(i));
        out << QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
    }
    return out;
}

void Document::setLayerFit(const QString& fit) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    const auto parsed = ldoc::fit_mode_from_string(to_std(fit));
    if (!s || !n || !parsed)
        return;
    ldoc::Layout l = n->layout;
    l.fit = *parsed;
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setLayerBoxSize(double w, double h) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.size = ldoc::Vec2{std::max(0.0, w), std::max(0.0, h)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setLayerCrop(double left, double top, double right, double bottom) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.crop = ldoc::CropRect{std::max(0.0, left), std::max(0.0, top), std::max(0.0, right),
                            std::max(0.0, bottom)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setLayerAlignment(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.alignment = ldoc::Vec2{std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setLayerBlendMode(const QString& mode) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_layer();
    const auto parsed = ldoc::blend_mode_from_string(to_std(mode));
    if (!s || !n || !parsed)
        return;
    ldoc::Blend b = n->blend;
    b.mode = *parsed;
    run(ldoc::SetNodeBlend{s->id, n->id, b});
}

// ---- history and persistence -------------------------------------------------------------------

const ldoc::Project& Document::projectForBindings() const { return session_->project(); }

void Document::runCommand(const ldoc::Command& command) { run(command); }

void Document::undo() {
    if (const auto r = session_->undo(); !r)
        emit notice(qstr(r.error()));
}

void Document::redo() {
    if (const auto r = session_->redo(); !r)
        emit notice(qstr(r.error()));
}

void Document::save() {
    if (session_->path().empty()) {
        emit notice(QStringLiteral("This project has not been given a location yet."));
        return;
    }
    if (const auto r = session_->save(); !r)
        emit notice(QStringLiteral("Could not save: %1").arg(qstr(r.error())));
}

} // namespace libertas::ui
