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

/// A node's kind as a string for QML. Since the format bump this is simply the format's own
/// word: `to_string` says "source" where it used to say "layer" (ADR-0028).
QString kind_str(ldoc::NodeKind k) {
    const auto s = ldoc::to_string(k);
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
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

    // Which scene each canvas starts on is settled by rebuild(), which does it per canvas and
    // has to anyway -- a canvas can gain its first scene at any time.
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

QString Document::canvas_key(const QString& canvasId) const {
    // Empty means the first canvas. The layout's plain "scenes"/"sources" panel ids carry no
    // canvas, and Main is the one canvas guaranteed to exist and to stay at index 0, so it is
    // the one that can be named by omission.
    if (!canvasId.isEmpty())
        return canvasId;
    const auto& canvases = session_->project().canvases;
    return canvases.empty() ? QString{} : id_str(canvases.front().id);
}

QAbstractItemModel* Document::scenesFor(const QString& canvasId) {
    const QString key = canvas_key(canvasId);
    auto it = scenes_by_canvas_.find(key);
    if (it == scenes_by_canvas_.end()) {
        // Parented to this, so the QML engine treats it as C++-owned and a binding that
        // outlives its panel does not take the model with it.
        it = scenes_by_canvas_.emplace(key, new SceneModel(this)).first;
    }
    return it->second;
}

QAbstractItemModel* Document::nodesFor(const QString& canvasId) {
    const QString key = canvas_key(canvasId);
    auto it = nodes_by_canvas_.find(key);
    if (it == nodes_by_canvas_.end())
        it = nodes_by_canvas_.emplace(key, new NodeModel(this)).first;
    return it->second;
}

QString Document::selectedSceneOn(const QString& canvasId) const {
    const auto it = selected_scene_.find(canvas_key(canvasId));
    return it == selected_scene_.end() ? QString{} : it->second;
}

void Document::selectSceneOn(const QString& canvasId, const QString& sceneId) {
    const QString key = canvas_key(canvasId);
    const bool same_canvas = current_canvas_ == key ||
                             (current_canvas_.isEmpty() && key == canvas_key(QString{}));
    if (selectedSceneOn(key) == sceneId && same_canvas)
        return;
    selected_scene_[key] = sceneId;
    selected_node_.clear();
    // The canvas the shell is working on follows the click. Picking a row in "Scenes:
    // Vertical" is how an operator says "Vertical now" -- requiring a second click in the
    // canvas list first would be a step that exists only because of how this is stored.
    if (!same_canvas) {
        current_canvas_ = key;
        emit canvasChanged();
    }
    rebuild();
}

QString Document::canvasNameOf(const QString& canvasId) const {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* c = parsed ? session_->project().canvas(*parsed) : nullptr;
    return c ? qstr(c->name) : QString{};
}

QSize Document::canvasSizeOf(const QString& canvasId) const {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* c = parsed ? session_->project().canvas(*parsed) : nullptr;
    return c ? QSize(c->width, c->height) : QSize{};
}

QStringList Document::canvasIds() const {
    QStringList out;
    for (const ldoc::Canvas& c : session_->project().canvases)
        out << id_str(c.id);
    return out;
}

QStringList Document::canvasNames() const {
    QStringList out;
    for (const ldoc::Canvas& c : session_->project().canvases)
        out << qstr(c.name);
    return out;
}

const ldoc::Scene* Document::current_scene() const {
    const auto id = parse_id(selectedSceneOn(current_canvas_));
    return id ? session_->project().scene(*id) : nullptr;
}

const ldoc::Scene* Document::scene_of_canvas(const QString& canvasId) const {
    const auto id = parse_id(selectedSceneOn(canvasId));
    return id ? session_->project().scene(*id) : nullptr;
}

const ldoc::Node* Document::current_node() const {
    const ldoc::Scene* s = current_scene();
    const auto id = parse_id(selected_node_);
    if (!s || !id)
        return nullptr;
    return ldoc::find_node(s->roots, *id);
}

const ldoc::Canvas* Document::current_canvas() const {
    const auto& canvases = session_->project().canvases;
    if (canvases.empty())
        return nullptr;
    // Resolved rather than stored: the selection is an id the operator picked, and the canvas
    // behind it can be removed by an undo or by another panel. Falling back to the first one
    // means the shell is never working on a canvas that is not there.
    if (const auto wanted = parse_id(current_canvas_)) {
        for (const ldoc::Canvas& c : canvases) {
            if (c.id == *wanted)
                return &c;
        }
    }
    return &canvases.front();
}

QString Document::currentCanvasName() const {
    const ldoc::Canvas* c = current_canvas();
    return c ? qstr(c->name) : QString{};
}

bool Document::currentCanvasIsMain() const {
    const auto& canvases = session_->project().canvases;
    const ldoc::Canvas* c = current_canvas();
    return !canvases.empty() && c && c->id == canvases.front().id;
}

int Document::currentCanvasSceneCount() const {
    const ldoc::Canvas* c = current_canvas();
    if (!c)
        return 0;
    int n = 0;
    for (const ldoc::Scene& s : session_->project().scenes) {
        if (s.canvas == c->id)
            ++n;
    }
    return n;
}

void Document::selectCanvas(const QString& raw) {
    // Normalised, so selectCanvas("") -- what a panel bound to the first canvas passes --
    // means that canvas rather than "no canvas".
    const QString id = canvas_key(raw);
    if (current_canvas_ == id)
        return;
    current_canvas_ = id;
    // Nothing to strand: each canvas keeps its own place in its own scene list, so switching
    // canvas and switching back lands where the operator left off. Only the node selection is
    // global, and it belongs to a scene that is no longer being shown.
    selected_node_.clear();
    emit canvasChanged();
    rebuild();
}

namespace {

/// Keep a generated name in step with the settings it describes.
///
/// An encode's name is a description -- "1080p60 8 Mbps" -- so leaving it alone when the
/// bitrate changes leaves a name that lies, and the name is the handle: it is what the outputs
/// rows print when they say two destinations share an encode. But a name the operator typed is
/// theirs and must survive.
///
/// Telling the two apart needs no extra field. If the current name is still exactly what the
/// generator would have produced for the settings as they were, nobody has renamed it.
void retitle_if_auto(const ldoc::Project& p, const ldoc::Encode& before, ldoc::Encode& after) {
    if (before.name == ldoc::default_encode_name(p, before))
        after.name = ldoc::default_encode_name(p, after);
}

} // namespace

QVariantMap Document::nodeProperties() const {
    QVariantMap m;
    const ldoc::Node* n = current_node();
    if (!n)
        return m;
    m["id"] = id_str(n->id);
    m["name"] = qstr(n->name);
    m["kind"] = kind_str(n->kind());
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

    // Audio, for the mixer's selected-node strip. A source node with no bus is unrouted, which is a
    // different thing from muted -- so both are reported rather than collapsed into one flag.
    if (const auto* placed = std::get_if<ldoc::SourceData>(&n->data)) {
        // Asked of the source's *type*, not assumed. Said true for everything until now, which
        // is why a colour card offered to be routed to a bus.
        const ldoc::Source* src =
            placed->source ? session_->project().source(*placed->source) : nullptr;
        m["hasAudio"] = src && ldoc::has_audio(src->type);
        // What the settings dialog is opened against. Empty when the node is bound to nothing,
        // which is what disables the gear rather than opening an empty panel.
        m["sourceId"] = placed->source ? id_str(*placed->source) : QString{};
        m["audioBus"] = placed->audio.bus ? id_str(*placed->audio.bus) : QString{};
        m["audioGain"] = placed->audio.gain_db;
        m["audioMute"] = placed->audio.mute;
        m["audioSendCount"] = static_cast<int>(placed->audio.sends.size());
        QString bus_name;
        if (placed->audio.bus) {
            const ldoc::Project& p = session_->project();
            bus_name = QStringLiteral("(missing bus)");
            for (const ldoc::Bus& b : p.buses) {
                if (b.id == *placed->audio.bus)
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
    // validated on every change rather than trusted. Doing it afterwards left the Sources list
    // built from a scene that had just been removed -- empty, while a different scene was
    // selected -- and nothing triggered another rebuild, so the panel stayed empty until the
    // operator clicked something.
    // Two directions, not one. Losing the selected scene has to move the selection, and so does
    // *gaining* a scene when there is none selected -- which happens when an undo brings back
    // everything that was deleted. Without the second case the panels stay on "no scene
    // selected" with a full scene list beside them, and nothing but a click gets out of it.
    for (const ldoc::Canvas& c : p.canvases) {
        const QString key = id_str(c.id);
        const ldoc::Scene* held = scene_of_canvas(key);
        if (held && held->canvas == c.id)
            continue;
        // Land on this canvas's first scene, or on nothing if it has none yet.
        QString first;
        for (const ldoc::Scene& sc : p.scenes) {
            if (sc.canvas == c.id) {
                first = id_str(sc.id);
                break;
            }
        }
        selected_scene_[key] = first;
        selected_node_.clear();
    }
    if (!selected_node_.isEmpty() && !current_node())
        selected_node_.clear();

    // Scenes belong to a canvas (ADR-0027), and each canvas has its own pair of panels --
    // "Scenes: Main", "Scenes: Vertical" -- so each gets its own model rather than one model
    // filtered to whichever canvas is current.
    const ldoc::Id main_canvas = p.canvases.empty() ? ldoc::Id{} : p.canvases.front().id;

    const auto scene_row = [&](const ldoc::Scene& s) {
        SceneRow row;
        row.id = id_str(s.id);
        row.name = qstr(s.name);
        int count = 0;
        ldoc::visit(s.roots, [&count](const ldoc::Node&) { ++count; });
        row.nodeCount = count;
        for (const ldoc::Deck& d : p.decks) {
            row.isProgram = row.isProgram || (d.program_scene && *d.program_scene == s.id);
            row.isPreview = row.isPreview || (d.preview_scene && *d.preview_scene == s.id);
        }
        for (const ldoc::Canvas& c : p.canvases)
            row.isEmergency = row.isEmergency || (c.emergency_scene && *c.emergency_scene == s.id);
        row.isFavourite = s.favourite;
        // How many Main scenes bring this one up. A link always runs from Main, so this is
        // zero on every Main scene by construction, and the panel draws no chain there.
        for (const ldoc::SceneLink& l : p.scene_links) {
            if (l.to == s.id)
                ++row.linkCount;
        }
        row.canLink = !p.canvases.empty() && s.canvas != main_canvas;
        return row;
    };

    std::map<QString, std::vector<SceneRow>> scenes_per_canvas;
    for (const ldoc::Canvas& c : p.canvases)
        scenes_per_canvas[id_str(c.id)]; // every canvas gets a list, including an empty one

    std::vector<SceneRow> favourites;
    for (const ldoc::Scene& s : p.scenes) {
        SceneRow row = scene_row(s);
        // Favourites deliberately span canvases: the panel exists so a live workspace can close
        // the scene list entirely, and the scenes worth reaching in a hurry are not all on one
        // canvas.
        if (row.isFavourite)
            favourites.push_back(row);
        scenes_per_canvas[id_str(s.canvas)].push_back(std::move(row));
    }
    favourites_.set_rows(std::move(favourites));
    for (auto& [canvas, rows] : scenes_per_canvas) {
        auto* model = static_cast<SceneModel*>(scenesFor(canvas));
        model->set_rows(std::move(rows));
    }
    // A canvas that has gone keeps its model, emptied. A panel can outlive its canvas by a
    // frame, and an empty list is a better thing for it to be showing than the last canvas's.
    for (auto& [canvas, model] : scenes_by_canvas_) {
        if (!scenes_per_canvas.contains(canvas))
            model->set_rows({});
    }

    // One node list per canvas: the Sources panel of a canvas shows that canvas's selected
    // scene, so two canvases on screen at once show two different trees.
    const auto node_rows = [&](const ldoc::Scene* s) {
    std::vector<NodeRow> rows;
    if (s) {
        // Flattened depth-first, which is what the Sources panel is: the tree with indentation.
        const std::function<void(const std::vector<ldoc::Node>&, int)> walk =
            [&](const std::vector<ldoc::Node>& nodes, int depth) {
                for (const ldoc::Node& n : nodes) {
                    NodeRow row;
                    row.id = id_str(n.id);
                    row.name = qstr(n.name);
                    row.kind = kind_str(n.kind());
                    row.depth = depth;
                    row.visible = n.visible;
                    row.locked = n.locked;
                    row.isGroup = n.is_group();
                    row.effectCount = static_cast<int>(n.effects.size());
                    if (const auto* placed = std::get_if<ldoc::SourceData>(&n.data)) {
                        if (placed->source) {
                            // The id is carried even when the source has gone: a row that says
                            // "(missing source)" with no id would look identical to an unbound
                            // one, and they want different things done to them.
                            row.sourceId = id_str(*placed->source);
                            if (const ldoc::Source* src = p.source(*placed->source))
                                row.sourceName = qstr(src->name);
                            else
                                row.sourceName = QStringLiteral("(missing source)");
                        }
                    }
                    rows.push_back(std::move(row));
                    if (const auto* kids = n.children())
                        walk(*kids, depth + 1);
                }
            };
        walk(s->roots, 0);
    }
    return rows;
    };

    for (const ldoc::Canvas& c : p.canvases) {
        const QString key = id_str(c.id);
        auto* model = static_cast<NodeModel*>(nodesFor(key));
        model->set_rows(node_rows(scene_of_canvas(key)));
    }
    for (auto& [canvas, model] : nodes_by_canvas_) {
        if (!scenes_per_canvas.contains(canvas))
            model->set_rows({});
    }

    std::vector<SourceRow> sources;
    for (const ldoc::Source& src : p.sources) {
        SourceRow row;
        row.id = id_str(src.id);
        row.name = qstr(src.name);
        row.type = qstr(src.type);
        row.selfContained = ldoc::is_self_contained(src.type);
        for (const ldoc::Scene& s : p.scenes) {
            ldoc::visit(s.roots, [&](const ldoc::Node& n) {
                const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
                if (placed && placed->source && *placed->source == src.id)
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
        row.monitor = b.monitor;
        row.hidden = b.hidden;
        row.levelLocked = b.level_locked;
        row.channels = b.channels;
        row.sendCount = static_cast<int>(b.sends.size());
        // Every scene, not just the selected one: a bus is project-level, and the question the
        // strip answers is whether anything in the show feeds it at all.
        for (const ldoc::Scene& sc : p.scenes) {
            ldoc::visit(sc.roots, [&](const ldoc::Node& n) {
                const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
                if (placed && placed->audio.bus && *placed->audio.bus == b.id)
                    ++row.feedCount;
            });
        }
        if (const ldoc::Node* sel = current_node()) {
            const auto* placed = std::get_if<ldoc::SourceData>(&sel->data);
            row.carriesSelection = placed && placed->audio.bus && *placed->audio.bus == b.id;
        }
        buses.push_back(std::move(row));
    }
    buses_.set_rows(std::move(buses));

    // The source strips, from the scene on screen. Rebuilt from the selected scene rather than
    // the program one: this panel is mostly read while building a scene, and faders that
    // belonged to whatever is on air would be the wrong set at exactly the moment they matter.
    std::vector<MixerStripRow> strips;
    if (const ldoc::Scene* shown = current_scene()) {
        ldoc::visit(shown->roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (!placed)
                return;
            const ldoc::Source* src = placed->source ? p.source(*placed->source) : nullptr;
            // A node bound to nothing has no type to ask, so it gets no strip. Unbound is a
            // state the Sources panel already reports; a silent fader would say it twice and
            // less clearly.
            if (!src || !ldoc::has_audio(src->type))
                return;
            MixerStripRow row;
            row.id = id_str(n.id);
            row.nodeId = row.id;
            row.sceneId = id_str(shown->id);
            row.name = qstr(n.name);
            if (placed->audio.bus) {
                row.busId = id_str(*placed->audio.bus);
                const ldoc::Bus* b = p.bus(*placed->audio.bus);
                row.busName = b ? qstr(b->name) : QStringLiteral("(missing bus)");
            }
            row.gainDb = placed->audio.gain_db;
            row.mute = placed->audio.mute;
            row.monitor = placed->audio.monitor;
            row.hidden = placed->audio.hidden;
            row.levelLocked = placed->audio.level_locked;
            row.selected = row.nodeId == selected_node_;
            strips.push_back(std::move(row));
        });
    }
    mixer_strips_.set_rows(std::move(strips));

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

    std::vector<CanvasRow> canvas_rows;
    for (const ldoc::Canvas& c : p.canvases) {
        CanvasRow row;
        row.id = id_str(c.id);
        row.name = qstr(c.name);
        row.pixelWidth = c.width;
        row.pixelHeight = c.height;
        row.fps = c.fps.den == 0 ? 0.0
                                 : static_cast<double>(c.fps.num) / static_cast<double>(c.fps.den);
        const auto cs = ldoc::to_string(c.color_space);
        row.colorSpace = QString::fromUtf8(cs.data(), static_cast<qsizetype>(cs.size()));
        row.isMain = c.id == p.canvases.front().id;
        row.hasEmergencyScene = c.emergency_scene.has_value();
        for (const ldoc::Scene& sc : p.scenes) {
            if (sc.canvas == c.id)
                ++row.sceneCount;
        }
        canvas_rows.push_back(std::move(row));
    }
    canvases_.set_rows(std::move(canvas_rows));

    // The encode list, which is the outputs manager's spine: one row per encode with its
    // destinations hanging off it. This is the inspector's "N concurrent encode(s) asked of the
    // GPU" in a shape the operator can act on (ADR-0029).
    std::vector<EncodeRow> encode_rows;
    for (const ldoc::Encode& e : p.encodes) {
        EncodeRow row;
        row.id = id_str(e.id);
        row.name = qstr(e.name);
        row.canvasId = id_str(e.canvas);
        row.canvasName = canvas_name(e.canvas);
        const ldoc::Canvas* c = p.canvas(e.canvas);
        const double w = e.rescale ? e.rescale->x : (c ? c->width : 0);
        const double h = e.rescale ? e.rescale->y : (c ? c->height : 0);
        row.resolution = QStringLiteral("%1x%2")
                             .arg(static_cast<long long>(w))
                             .arg(static_cast<long long>(h));
        row.codec = qstr(e.encoder.codec);
        row.implementation = qstr(e.encoder.implementation);
        row.bitrateKbps = e.encoder.bitrate_kbps;
        row.preset = qstr(e.encoder.preset);
        for (const ldoc::Output& o : p.outputs) {
            if (o.encode == e.id)
                row.destinations.push_back(qstr(o.name));
        }
        encode_rows.push_back(std::move(row));
    }
    encodes_.set_rows(std::move(encode_rows));

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
        // An output names an encode and the encode names the canvas, so both are one hop
        // away now (ADR-0029). The row shows the encode's settings because that is what this
        // destination actually receives -- and if another output names the same encode, these
        // are the same numbers on both rows, which is true rather than a display bug.
        const ldoc::Encode* enc = p.encode(o.encode);
        row.canvasName = enc ? canvas_name(enc->canvas) : QStringLiteral("(missing encode)");
        row.encodeName = enc ? qstr(enc->name) : QString{};
        // The canvas name says the wrong resolution whenever the encode rescales, and
        // rescaling is the ordinary case: 1080p to Twitch off a 1440p canvas. Show what
        // actually leaves the encoder.
        if (enc) {
            const ldoc::Canvas* c = p.canvas(enc->canvas);
            const double w = enc->rescale ? enc->rescale->x : (c ? c->width : 0);
            const double h = enc->rescale ? enc->rescale->y : (c ? c->height : 0);
            row.resolution = QStringLiteral("%1x%2")
                                 .arg(static_cast<long long>(w))
                                 .arg(static_cast<long long>(h));
        }
        // An encode feeding more than one destination is the whole point of ADR-0029, and
        // two rows carrying the same numbers do not say it -- two separate encodes
        // configured alike look exactly the same. Name the others.
        if (enc) {
            for (const ldoc::Output& other : p.outputs) {
                if (other.id != o.id && other.encode == o.encode)
                    row.sharedWith.push_back(qstr(other.name));
            }
        }
        row.codec = enc ? qstr(enc->encoder.codec) : QString{};
        row.implementation = enc ? qstr(enc->encoder.implementation) : QString{};
        row.bitrateKbps = enc ? enc->encoder.bitrate_kbps : 0;
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
    // A scene names its canvas, so a bare "select this scene" can route itself. Keeps every
    // existing caller -- and the favourites panel, which deliberately spans canvases --
    // working without each one having to know which canvas it just reached into.
    const auto parsed = parse_id(id);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    selectSceneOn(s ? id_str(s->canvas) : current_canvas_, id);
}

void Document::selectNode(const QString& id) {
    // A plain select collapses the selection to one. Every caller that was written before
    // multi-select means "this node and nothing else", and that is still what it does.
    if (selected_node_ == id && also_selected_.isEmpty())
        return;
    selected_node_ = id;
    also_selected_.clear();
    selection_anchor_ = id;
    emit selectionChanged();
}

QStringList Document::selectedNodes() const {
    QStringList out;
    if (selected_node_.isEmpty() && also_selected_.isEmpty())
        return out;
    // In scene order rather than click order: what acts on this -- grouping especially --
    // cares which is topmost, and the operator reads the list top to bottom.
    const ldoc::Scene* s = current_scene();
    if (!s) {
        if (!selected_node_.isEmpty())
            out << selected_node_;
        return out << also_selected_;
    }
    ldoc::visit(s->roots, [&](const ldoc::Node& n) {
        const QString id = id_str(n.id);
        if (id == selected_node_ || also_selected_.contains(id))
            out << id;
    });
    return out;
}

void Document::selectNodeExtended(const QString& canvasId, const QString& nodeId, bool ctrl,
                                  bool shift) {
    const QString key = canvas_key(canvasId);
    if (current_canvas_ != key) {
        current_canvas_ = key;
        emit canvasChanged();
        // A selection does not survive moving to another canvas's panel: the nodes it names
        // are not in the scene being shown.
        selected_node_.clear();
        also_selected_.clear();
        selection_anchor_.clear();
    }

    if (!ctrl && !shift) {
        selectNode(nodeId);
        return;
    }

    if (ctrl) {
        // Toggle. Taking away the node panels are following moves them to whatever is left,
        // rather than leaving them pointed at something no longer selected.
        if (nodeId == selected_node_) {
            selected_node_ = also_selected_.isEmpty() ? QString{} : also_selected_.takeLast();
        } else if (also_selected_.removeAll(nodeId) == 0) {
            if (!selected_node_.isEmpty())
                also_selected_ << selected_node_;
            selected_node_ = nodeId;
        }
        selection_anchor_ = selected_node_;
        emit selectionChanged();
        return;
    }

    // Shift: everything between the anchor and here, in scene order. A flat run through the
    // tree, so a range that starts outside a group and ends inside one takes the nodes in
    // between -- which is what the list on screen shows and therefore what was meant.
    const ldoc::Scene* s = current_scene();
    if (!s || selection_anchor_.isEmpty()) {
        selectNode(nodeId);
        return;
    }
    QStringList order;
    ldoc::visit(s->roots, [&](const ldoc::Node& n) { order << id_str(n.id); });
    const qsizetype from = order.indexOf(selection_anchor_);
    const qsizetype to = order.indexOf(nodeId);
    if (from < 0 || to < 0) {
        selectNode(nodeId);
        return;
    }
    also_selected_.clear();
    for (qsizetype i = std::min(from, to); i <= std::max(from, to); ++i) {
        if (order[i] != nodeId)
            also_selected_ << order[i];
    }
    // The clicked node leads, so the panels follow where the pointer last was. The anchor
    // stays put, so dragging the range back and forth keeps measuring from the same end.
    selected_node_ = nodeId;
    emit selectionChanged();
}

// ---- canvases ---------------------------------------------------------------------------------

void Document::addCanvas(const QString& name) {
    const ldoc::Project& p = session_->project();
    ldoc::Canvas c;
    c.id = ldoc::Id::generate();
    c.name = name.isEmpty() ? std::string{"Canvas"} : to_std(name);
    // A second canvas is nearly always the vertical one, but guessing a portrait size would be
    // a guess; it starts as a copy of Main's shape, which is the only thing known to be a
    // shape this show already uses. The manager edits it.
    if (!p.canvases.empty()) {
        c.width = p.canvases.front().width;
        c.height = p.canvases.front().height;
        c.fps = p.canvases.front().fps;
        c.color_space = p.canvases.front().color_space;
    }
    const QString id = id_str(c.id);
    // Never at index 0: that is Main's place and the command refuses it anyway (ADR-0027).
    run(ldoc::AddCanvas{std::move(c), p.canvases.size()});
    selectCanvas(id);
}

void Document::removeCanvas(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    // Dropping the selection first. The canvas is about to stop existing, and current_canvas()
    // falling back to Main is the right answer -- but only if nothing is still naming the one
    // that went.
    selected_scene_.erase(id);
    if (current_canvas_ == id) {
        current_canvas_.clear();
        selected_node_.clear();
        emit canvasChanged();
    }
    run(ldoc::RemoveCanvas{*parsed});
}

void Document::edit_canvas(const QString& id, const std::function<void(ldoc::Canvas&)>& change) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Canvas* existing = session_->project().canvas(*parsed);
    if (!existing)
        return;
    ldoc::Canvas next = *existing;
    change(next);
    if (next == *existing)
        return;
    run(ldoc::SetCanvas{std::move(next)});
}

void Document::renameCanvas(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) { c.name = to_std(name); });
}

void Document::setCanvasSize(const QString& id, int width, int height) {
    if (width <= 0 || height <= 0)
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) {
        c.width = width;
        c.height = height;
    });
}

void Document::setCanvasFps(const QString& id, int num, int den) {
    if (num <= 0 || den <= 0)
        return;
    edit_canvas(id, [&](ldoc::Canvas& c) {
        c.fps.num = num;
        c.fps.den = den;
    });
}

// ---- scene links ------------------------------------------------------------------------------

int Document::sceneLinkCount(const QString& sceneId) const {
    const auto parsed = parse_id(sceneId);
    if (!parsed)
        return 0;
    int n = 0;
    for (const ldoc::SceneLink& l : session_->project().scene_links) {
        if (l.to == *parsed)
            ++n;
    }
    return n;
}

QVariantList Document::mainScenesForLink(const QString& sceneId) const {
    QVariantList out;
    const ldoc::Project& p = session_->project();
    const auto parsed = parse_id(sceneId);
    if (!parsed || p.canvases.empty())
        return out;
    const ldoc::Scene* target = p.scene(*parsed);
    if (!target || target->canvas == p.canvases.front().id)
        return out; // Main has no link control at all.

    for (const ldoc::Scene& s : p.scenes) {
        if (s.canvas != p.canvases.front().id)
            continue;
        QVariantMap row;
        row[QStringLiteral("id")] = id_str(s.id);
        row[QStringLiteral("name")] = qstr(s.name);
        bool ticked = false;
        QString taken_by;
        for (const ldoc::SceneLink& l : p.scene_links) {
            if (l.from != s.id)
                continue;
            if (l.to == target->id) {
                ticked = true;
                continue;
            }
            // Already cueing something else on *this* canvas. Ticking it here moves the link,
            // and the popup says so before the operator presses Link (ADR-0027 amendment).
            const ldoc::Scene* other = p.scene(l.to);
            if (other && other->canvas == target->canvas)
                taken_by = qstr(other->name);
        }
        row[QStringLiteral("ticked")] = ticked;
        row[QStringLiteral("takenBy")] = taken_by;
        out.push_back(row);
    }
    return out;
}

void Document::commitSceneLinks(const QString& sceneId, const QStringList& mainSceneIds) {
    const ldoc::Project& p = session_->project();
    const auto parsed = parse_id(sceneId);
    if (!parsed || p.canvases.empty())
        return;
    const ldoc::Scene* target = p.scene(*parsed);
    if (!target)
        return;

    std::vector<ldoc::Id> wanted;
    for (const QString& s : mainSceneIds) {
        if (const auto id = parse_id(s))
            wanted.push_back(*id);
    }
    const auto is_wanted = [&](const ldoc::Id& id) {
        return std::find(wanted.begin(), wanted.end(), id) != wanted.end();
    };

    std::vector<ldoc::Command> steps;
    QStringList moved;

    // Removals first, and that ordering is the whole trick. A tick that moves a link needs the
    // old one gone before the new one is added, or the one-cue-per-canvas rule refuses it --
    // and a batch that is refused half way through is a batch that did nothing.
    for (const ldoc::SceneLink& l : p.scene_links) {
        const ldoc::Scene* from = p.scene(l.from);
        const ldoc::Scene* to = p.scene(l.to);
        if (!from || !to)
            continue;
        if (l.to == target->id && !is_wanted(l.from)) {
            steps.push_back(ldoc::RemoveSceneLink{l.id}); // unticked
            continue;
        }
        if (l.to != target->id && to->canvas == target->canvas && is_wanted(l.from)) {
            steps.push_back(ldoc::RemoveSceneLink{l.id}); // moved here from another scene
            moved.push_back(QStringLiteral("%1 (was bringing up %2)")
                                .arg(qstr(from->name), qstr(to->name)));
        }
    }

    std::size_t at = p.scene_links.size();
    for (const ldoc::Id& from : wanted) {
        bool already = false;
        for (const ldoc::SceneLink& l : p.scene_links)
            already = already || (l.from == from && l.to == target->id);
        if (already)
            continue;
        steps.push_back(ldoc::AddSceneLink{ldoc::SceneLink{ldoc::Id::generate(), from, target->id},
                                           at});
        ++at;
    }

    if (steps.empty())
        return;
    // One undo step for the whole tick-list, which is what "Link" means: the popup has one
    // commit point, so undo has one too.
    run(ldoc::make_batch(std::move(steps), "Link scenes"));
    if (!moved.isEmpty())
        emit notice(QStringLiteral("Moved to \"%1\": %2")
                        .arg(qstr(target->name), moved.join(QStringLiteral("; "))));
}

// ---- scenes -----------------------------------------------------------------------------------

void Document::addScene(const QString& name) {
    addSceneOn(current_canvas_, name);
}

void Document::selectNodeOn(const QString& canvasId, const QString& nodeId) {
    const QString key = canvas_key(canvasId);
    if (current_canvas_ != key) {
        current_canvas_ = key;
        emit canvasChanged();
    }
    selectNode(nodeId);
}

void Document::addSceneOn(const QString& canvasId, const QString& name) {
    const auto parsed = parse_id(canvas_key(canvasId));
    const ldoc::Canvas* canvas = parsed ? session_->project().canvas(*parsed) : nullptr;
    if (!canvas)
        return;
    ldoc::Scene s;
    s.id = ldoc::Id::generate();
    s.name = name.isEmpty() ? std::string{"Scene"} : to_std(name);
    // The canvas whose list the + was pressed in. Every canvas has its own scenes panel, so
    // there is no guessing left to do here (ADR-0027).
    s.canvas = canvas->id;
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
    // The canvas comes from the scene, not from whichever canvas happens to be current. A
    // scene belongs to exactly one canvas, so "which canvas is this the safety net for" has
    // one answer and it is not a question the operator should be able to get wrong by having
    // the wrong panel focused (ADR-0025, ADR-0027).
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    const ldoc::Canvas* canvas = s ? session_->project().canvas(s->canvas) : nullptr;
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


QVariantList Document::addableKinds() const {
    QVariantList out = sourceTypes();
    // Last, after the source types, because it is the odd one: picking it adds a Precomp node
    // rather than a source (ADR-0034). Listed here at all because this dialog answers "what
    // goes in a scene", and another scene is one of the answers -- where OBS puts it too.
    QVariantMap scene;
    scene["id"] = QStringLiteral("scene");
    scene["title"] = QStringLiteral("Scene");
    scene["icon"] = QString::fromUtf8("\u25F0");
    scene["selfContained"] = false;
    out.append(scene);
    return out;
}

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

/// Where a new source node goes: inside the selected group if one is selected, otherwise at the
/// top.
struct Placement {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

} // namespace

void Document::addNewSourceToScene(const QString& name, const QString& type, bool visible) {
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

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, src.name);
    node.visible = visible;
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddSource{std::move(src), p.sources.size()},
                          ldoc::AddNode{scene->id, where.parent, where.index, std::move(node)},
                          ldoc::SetNodeSource{scene->id, node_id, source_id}},
                         std::format("Add {}", name.isEmpty() ? std::string{"source"}
                                                              : to_std(name))));
    selectNode(id_str(node_id));
}

void Document::addExistingSourceToScene(const QString& sourceId, bool visible) {
    const ldoc::Scene* scene = current_scene();
    const auto parsed = parse_id(sourceId);
    if (!scene || !parsed)
        return;
    const ldoc::Source* existing = session_->project().source(*parsed);
    if (!existing) {
        emit notice(QStringLiteral("That source is no longer in the project."));
        return;
    }

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, existing->name);
    node.visible = visible;
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::make_batch({ldoc::AddNode{scene->id, where.parent, where.index, std::move(node)},
                          ldoc::SetNodeSource{scene->id, node_id, *parsed}},
                         std::format("Add {}", existing->name)));
    selectNode(id_str(node_id));
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
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == src.id)
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

QStringList Document::scenesUsing(const QString& sourceId) const {
    const auto parsed = parse_id(sourceId);
    if (!parsed)
        return {};
    QStringList out;
    for (const ldoc::Scene& s : session_->project().scenes) {
        bool used = false;
        ldoc::visit(s.roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == *parsed)
                used = true;
        });
        // Once per scene, not once per placement: the question the dialog is asking is which
        // scenes change, and "Live, Live, Live" answers it worse than "Live".
        if (used)
            out << qstr(s.name);
    }
    return out;
}

void Document::removeSourceAndUses(const QString& id) {
    const auto parsed = parse_id(id);
    const ldoc::Project& p = session_->project();
    const ldoc::Source* src = parsed ? p.source(*parsed) : nullptr;
    if (!src)
        return;

    std::vector<ldoc::Command> steps;
    for (const ldoc::Scene& s : p.scenes) {
        std::vector<ldoc::Id> doomed;
        ldoc::visit(s.roots, [&](const ldoc::Node& n) {
            const auto* placed = std::get_if<ldoc::SourceData>(&n.data);
            if (placed && placed->source && *placed->source == *parsed)
                doomed.push_back(n.id);
        });
        for (const ldoc::Id& node : doomed)
            steps.push_back(ldoc::RemoveNode{s.id, node});
    }
    steps.push_back(ldoc::RemoveSource{*parsed});
    run(ldoc::make_batch(std::move(steps),
                         std::format("Delete source {}", src->name)));
}

QVariantList Document::scenesForPrecomp() const {
    const ldoc::Scene* into = current_scene();
    if (!into)
        return {};
    const ldoc::Project& p = session_->project();

    // Would showing `candidate` here eventually come back to the scene being added to? The
    // command refuses a loop; this asks the same question early, so a scene that cannot be
    // added is never offered and then rejected.
    std::unordered_set<ldoc::Id> seen;
    const std::function<bool(const ldoc::Id&)> reaches_into = [&](const ldoc::Id& at) {
        if (at == into->id)
            return true;
        if (!seen.insert(at).second)
            return false;
        const ldoc::Scene* s = p.scene(at);
        if (!s)
            return false;
        bool found = false;
        ldoc::visit(s->roots, [&](const ldoc::Node& n) {
            if (found)
                return;
            if (const auto* pre = std::get_if<ldoc::PrecompData>(&n.data))
                found = reaches_into(pre->scene);
        });
        return found;
    };

    QVariantList out;
    for (const ldoc::Scene& s : p.scenes) {
        if (s.id == into->id)
            continue;
        seen.clear();
        if (reaches_into(s.id))
            continue;
        // Shaped like a source tile, because it is drawn by the same delegate. The fields it
        // would otherwise lack are filled rather than left absent: an undeclared field renders
        // as the literal word "undefined", which the shell has shipped once already.
        QVariantMap m;
        m["id"] = id_str(s.id);
        m["name"] = qstr(s.name);
        const ldoc::Canvas* c = p.canvas(s.canvas);
        m["canvasName"] = c ? qstr(c->name) : QString{};
        m["type"] = QStringLiteral("scene");
        m["typeTitle"] = c ? qstr(c->name) : QStringLiteral("Scene");
        m["icon"] = QString::fromUtf8("\u25F0");
        m["swatch"] = QString{};
        m["selfContained"] = false;
        // How many *other* scenes already show this one, so an overlay in use reads as one.
        int shown_by = 0;
        for (const ldoc::Scene& other : p.scenes) {
            bool uses = false;
            ldoc::visit(other.roots, [&](const ldoc::Node& n) {
                if (const auto* pre = std::get_if<ldoc::PrecompData>(&n.data))
                    uses = uses || pre->scene == s.id;
            });
            if (uses)
                ++shown_by;
        }
        m["useCount"] = shown_by;
        out.append(m);
    }
    return out;
}

void Document::addSceneToScene(const QString& sceneId, bool visible) {
    const ldoc::Scene* into = current_scene();
    const auto parsed = parse_id(sceneId);
    if (!into || !parsed)
        return;
    const ldoc::Scene* shown = session_->project().scene(*parsed);
    if (!shown) {
        emit notice(QStringLiteral("That scene is no longer in the project."));
        return;
    }

    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Precomp, shown->name);
    node.visible = visible;
    ldoc::PrecompData data;
    data.scene = shown->id;
    // size stays (0,0): "the size of the canvas this is composited into", which is what lets
    // an overlay scene authored at one size sit correctly on a canvas of another.
    if (const ldoc::Canvas* c = session_->project().canvas(shown->canvas))
        data.color_space = c->color_space;
    node.data = std::move(data);
    const ldoc::Id node_id = node.id;

    Placement where;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        where.parent = sel->id;

    run(ldoc::AddNode{into->id, where.parent, where.index, std::move(node)});
    selectNode(id_str(node_id));
}

// ---- the clipboard ---------------------------------------------------------------------------

void Document::copySelectedNode() {
    const ldoc::Node* n = current_node();
    if (!n) {
        emit notice(QStringLiteral("Select a source to copy."));
        return;
    }
    copied_scene_.reset();
    copied_node_ = *n;
    emit notice(QStringLiteral("Copied %1.").arg(qstr(n->name)));
}

void Document::copyScene(const QString& sceneId) {
    const auto parsed = parse_id(sceneId);
    const ldoc::Scene* s = parsed ? session_->project().scene(*parsed) : nullptr;
    if (!s) {
        emit notice(QStringLiteral("Select a scene to copy."));
        return;
    }
    copied_node_.reset();
    copied_scene_ = *s;
    emit notice(QStringLiteral("Copied scene %1.").arg(qstr(s->name)));
}

QString Document::pasteDescription() const {
    if (copied_node_)
        return QStringLiteral("Paste %1").arg(qstr(copied_node_->name));
    if (copied_scene_)
        return QStringLiteral("Paste scene %1").arg(qstr(copied_scene_->name));
    return {};
}

int Document::pasteWouldMissCanvas() const {
    const ldoc::Canvas* c = current_canvas();
    if (!c)
        return 0;
    // A node sitting entirely past the right or bottom edge, or before the left or top one.
    // Counted rather than listed: the operator asked to be told that something is off, not
    // which things (ADR-0036).
    const auto misses = [c](const ldoc::Node& n) {
        const double w = n.layout.size.x > 0 ? n.layout.size.x : 0.0;
        const double h = n.layout.size.y > 0 ? n.layout.size.y : 0.0;
        return n.transform.position.x >= static_cast<double>(c->width) ||
               n.transform.position.y >= static_cast<double>(c->height) ||
               n.transform.position.x + w <= 0.0 || n.transform.position.y + h <= 0.0;
    };
    int off = 0;
    if (copied_node_) {
        ldoc::visit(*copied_node_, [&](const ldoc::Node& n) {
            if (misses(n))
                ++off;
        });
    } else if (copied_scene_) {
        ldoc::visit(copied_scene_->roots, [&](const ldoc::Node& n) {
            if (misses(n))
                ++off;
        });
    }
    return off;
}

void Document::paste() {
    if (copied_node_) {
        const ldoc::Scene* into = current_scene();
        if (!into) {
            emit notice(QStringLiteral("There is no scene to paste into."));
            return;
        }
        // Fresh ids, or the paste would collide with the node it was copied from -- which
        // AddNode refuses, and rightly.
        ldoc::Node node = ldoc::duplicate(*copied_node_);
        const ldoc::Id node_id = node.id;

        Placement where;
        if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
            where.parent = sel->id;
        run(ldoc::AddNode{into->id, where.parent, where.index, std::move(node)});
        selectNode(id_str(node_id));
        return;
    }

    if (!copied_scene_) {
        emit notice(QStringLiteral("There is nothing to paste."));
        return;
    }

    const ldoc::Canvas* c = current_canvas();
    if (!c) {
        emit notice(QStringLiteral("There is no canvas to paste onto."));
        return;
    }
    // The name travels. Scene names only have to be unique within a canvas, and matching names
    // across canvases are what make a scene link readable: Main's "Live" cues Vertical's
    // "Live" (ADR-0036).
    ldoc::Scene copy = ldoc::duplicate_scene(*copied_scene_, copied_scene_->name);
    copy.canvas = c->id;
    const ldoc::Id scene_id = copy.id;
    run(ldoc::AddScene{std::move(copy), session_->project().scenes.size()});
    selectScene(id_str(scene_id));
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

void Document::bindSelectedNode(const QString& sourceId) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    if (!std::holds_alternative<ldoc::SourceData>(n->data)) {
        emit notice(QStringLiteral("That is not a source, so nothing can be bound to it."));
        return;
    }
    run(ldoc::SetNodeSource{s->id, n->id,
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

void Document::setBusMonitor(const QString& id, bool monitor) {
    edit_bus(id, [monitor](ldoc::Bus& b) { b.monitor = monitor; });
}

int Document::hiddenStripCount() const {
    int n = 0;
    for (const BusRow& b : buses_.rows()) {
        if (b.hidden)
            ++n;
    }
    for (const MixerStripRow& r : mixer_strips_.rows()) {
        if (r.hidden)
            ++n;
    }
    return n;
}

void Document::setBusHidden(const QString& id, bool hidden) {
    edit_bus(id, [hidden](ldoc::Bus& b) { b.hidden = hidden; });
}

void Document::setBusLevelLocked(const QString& id, bool locked) {
    edit_bus(id, [locked](ldoc::Bus& b) { b.level_locked = locked; });
}

/// Same shape for the selected source node's own routing, for the same reason.
void Document::edit_node_audio(const QString& sceneId, const QString& nodeId,
                               const std::function<void(ldoc::AudioRouting&)>& change,
                               std::string_view gesture) {
    const auto scene_parsed = parse_id(sceneId);
    const auto node_parsed = parse_id(nodeId);
    if (!scene_parsed || !node_parsed)
        return;
    const ldoc::Scene* s = session_->project().scene(*scene_parsed);
    if (!s)
        return;
    const ldoc::Node* n = ldoc::find_node(s->roots, *node_parsed);
    if (!n)
        return;
    const auto* placed = std::get_if<ldoc::SourceData>(&n->data);
    if (!placed) {
        emit notice(QStringLiteral("Only a source has audio of its own."));
        return;
    }
    ldoc::AudioRouting next = placed->audio;
    change(next);
    if (next == placed->audio)
        return;
    if (gesture.empty()) {
        run(ldoc::SetNodeAudio{s->id, n->id, std::move(next)});
        return;
    }
    const auto r = session_->run(ldoc::SetNodeAudio{s->id, n->id, std::move(next)}, gesture);
    if (!r)
        emit notice(qstr(r.error()));
}

void Document::edit_selected_audio(const std::function<void(ldoc::AudioRouting&)>& change) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    edit_node_audio(id_str(s->id), id_str(n->id), change);
}

void Document::setNodeGain(const QString& sceneId, const QString& nodeId, double gainDb,
                           bool dragging) {
    // One gesture per strip, as the bus faders do, so dragging two in turn is two undo steps
    // even if the operator never lets go in between. Closed by endGesture().
    const std::string gesture =
        dragging ? std::format("node.gain:{}", to_std(nodeId)) : std::string{};
    edit_node_audio(
        sceneId, nodeId, [gainDb](ldoc::AudioRouting& a) { a.gain_db = gainDb; }, gesture);
}

void Document::setNodeMute(const QString& sceneId, const QString& nodeId, bool mute) {
    edit_node_audio(sceneId, nodeId, [mute](ldoc::AudioRouting& a) { a.mute = mute; });
}

void Document::setNodeMonitor(const QString& sceneId, const QString& nodeId, bool monitor) {
    edit_node_audio(sceneId, nodeId, [monitor](ldoc::AudioRouting& a) { a.monitor = monitor; });
}

void Document::setNodeHidden(const QString& sceneId, const QString& nodeId, bool hidden) {
    edit_node_audio(sceneId, nodeId, [hidden](ldoc::AudioRouting& a) { a.hidden = hidden; });
}

void Document::setNodeLevelLocked(const QString& sceneId, const QString& nodeId, bool locked) {
    edit_node_audio(sceneId, nodeId,
                    [locked](ldoc::AudioRouting& a) { a.level_locked = locked; });
}

void Document::routeNode(const QString& sceneId, const QString& nodeId, const QString& busId) {
    edit_node_audio(sceneId, nodeId, [&](ldoc::AudioRouting& a) {
        a.bus = busId.isEmpty() ? std::nullopt : parse_id(busId);
    });
}

void Document::routeSelectedNode(const QString& busId) {
    edit_selected_audio([&](ldoc::AudioRouting& a) {
        a.bus = busId.isEmpty() ? std::nullopt : parse_id(busId);
    });
}

void Document::setSelectedNodeGain(double gainDb) {
    edit_selected_audio([gainDb](ldoc::AudioRouting& a) { a.gain_db = gainDb; });
}

void Document::setSelectedNodeMute(bool mute) {
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

int Document::encodeCount() const {
    return static_cast<int>(session_->project().encodes.size());
}

void Document::addOutput(const QString& name) {
    const ldoc::Project& p = session_->project();
    if (p.canvases.empty()) {
        emit notice(QStringLiteral("An output needs a canvas to encode."));
        return;
    }
    // Its own encode, not a share of an existing one. Sharing is the point of ADR-0029, but it
    // has to be asked for: a new output that silently joined another's bitstream would make
    // changing its bitrate change someone else's stream, which is the surprise this model
    // exists to make visible rather than to hide.
    ldoc::Encode e;
    e.id = ldoc::Id::generate();
    e.canvas = p.canvases.front().id;
    e.name = ldoc::default_encode_name(p, e);

    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = name.isEmpty() ? std::string{"Output"} : to_std(name);
    o.encode = e.id;
    // Disabled, with nowhere to send to. An output that arrived enabled and pointed at a URL
    // would be one keystroke from going somewhere the operator did not choose.
    o.enabled = false;

    // One undo step: nobody who presses Ctrl+Z after adding an output wants the encode left
    // behind.
    run(ldoc::make_batch({ldoc::AddEncode{std::move(e), p.encodes.size()},
                          ldoc::AddOutput{std::move(o), p.outputs.size()}},
                         "Add output"));
}

void Document::removeOutput(const QString& id) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Project& p = session_->project();
    const ldoc::Output* going = nullptr;
    for (const ldoc::Output& o : p.outputs) {
        if (o.id == *parsed)
            going = &o;
    }
    if (!going) {
        run(ldoc::RemoveOutput{*parsed});
        return;
    }
    // addOutput creates an encode and an output together, so removing the output has to take
    // the encode with it or the project accumulates encodes nothing sends -- and the inspector
    // counts those against the GPU, so a few add/remove cycles make the machine look busier
    // than it is. Only when this was the last output on it: a shared encode outlives any one
    // destination, which is the whole point of sharing (ADR-0029).
    const bool shared = std::any_of(p.outputs.begin(), p.outputs.end(),
                                    [&](const ldoc::Output& other) {
                                        return other.id != going->id &&
                                               other.encode == going->encode;
                                    });
    if (shared || !p.encode(going->encode)) {
        run(ldoc::RemoveOutput{*parsed});
        return;
    }
    // The output first: RemoveEncode is refused while anything still names it.
    run(ldoc::make_batch({ldoc::RemoveOutput{*parsed}, ldoc::RemoveEncode{going->encode}},
                         "Remove output"));
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

void Document::addEncode() {
    const ldoc::Project& p = session_->project();
    const ldoc::Canvas* c = current_canvas();
    if (!c) {
        emit notice(QStringLiteral("An encode needs a canvas to encode."));
        return;
    }
    ldoc::Encode e;
    e.id = ldoc::Id::generate();
    e.canvas = c->id;
    e.name = ldoc::default_encode_name(p, e);
    run(ldoc::AddEncode{std::move(e), p.encodes.size()});
}

void Document::setEncodeCanvas(const QString& id, const QString& canvasId) {
    const auto parsed = parse_id(id);
    const auto canvas = parse_id(canvas_key(canvasId));
    if (!parsed || !canvas)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    if (!session_->project().canvas(*canvas)) {
        emit notice(QStringLiteral("That canvas is no longer in the project."));
        return;
    }
    ldoc::Encode next = *existing;
    next.canvas = *canvas;
    // The generated name carries the resolution and the canvas decides it, so a name nobody
    // typed has to follow the move -- the same rule setEncodeRescale works by.
    retitle_if_auto(session_->project(), *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::renameEncode(const QString& id, const QString& name) {
    if (name.isEmpty())
        return;
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    ldoc::Encode next = *existing;
    next.name = to_std(name);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::setEncodeRescale(const QString& id, int width, int height) {
    const auto parsed = parse_id(id);
    if (!parsed)
        return;
    const ldoc::Encode* existing = session_->project().encode(*parsed);
    if (!existing)
        return;
    ldoc::Encode next = *existing;
    // Zero means "no rescale": send the canvas at its own size. A rescale equal to the canvas
    // is not the same thing stored -- it would survive a canvas resize and quietly pin the
    // encode to the old size.
    if (width <= 0 || height <= 0)
        next.rescale.reset();
    else
        next.rescale = ldoc::Vec2{static_cast<double>(width), static_cast<double>(height)};
    retitle_if_auto(session_->project(), *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
}

void Document::setOutputEncode(const QString& output_id, const QString& encode_id) {
    const auto out_parsed = parse_id(output_id);
    const auto enc_parsed = parse_id(encode_id);
    if (!out_parsed || !enc_parsed)
        return;
    const ldoc::Project& p = session_->project();
    const ldoc::Output* out = nullptr;
    for (const ldoc::Output& o : p.outputs) {
        if (o.id == *out_parsed)
            out = &o;
    }
    const ldoc::Encode* onto = p.encode(*enc_parsed);
    if (!out || !onto || out->encode == *enc_parsed)
        return;

    // The encode this destination is leaving. If it was the only one on it, moving away would
    // strand it -- the same leak as removing an output used to cause -- so it goes in the same
    // batch, as one undo step.
    const ldoc::Id leaving = out->encode;
    bool others_remain = false;
    for (const ldoc::Output& o : p.outputs)
        others_remain = others_remain || (o.id != out->id && o.encode == leaving);

    ldoc::Output next = *out;
    next.encode = *enc_parsed;
    std::vector<ldoc::Command> steps{ldoc::SetOutput{std::move(next)}};
    if (!others_remain && p.encode(leaving))
        steps.push_back(ldoc::RemoveEncode{leaving});
    run(ldoc::make_batch(std::move(steps), "Change encode"));
}

void Document::addOutputOnEncode(const QString& encode_id, const QString& name) {
    const auto parsed = parse_id(encode_id);
    if (!parsed)
        return;
    const ldoc::Project& p = session_->project();
    if (!p.encode(*parsed)) {
        emit notice(QStringLiteral("That encode is gone."));
        return;
    }
    ldoc::Output o;
    o.id = ldoc::Id::generate();
    o.name = name.isEmpty() ? std::string{"Output"} : to_std(name);
    o.encode = *parsed;
    // Disabled and pointed nowhere, exactly as addOutput makes one. Sharing an encode does not
    // make a destination any less of a thing that could go live by accident.
    o.enabled = false;
    run(ldoc::AddOutput{std::move(o), p.outputs.size()});
}

void Document::edit_encode_of(const QString& output_id,
                              const std::function<void(ldoc::Encode&)>& change) {
    const auto parsed = parse_id(output_id);
    if (!parsed)
        return;
    const ldoc::Project& p = session_->project();
    const ldoc::Output* out = nullptr;
    for (const ldoc::Output& o : p.outputs) {
        if (o.id == *parsed)
            out = &o;
    }
    if (!out)
        return;
    const ldoc::Encode* existing = p.encode(out->encode);
    if (!existing) {
        emit notice(QStringLiteral("'%1' has no encode to change.").arg(qstr(out->name)));
        return;
    }

    // The one thing this panel must not do quietly. Several destinations can share one
    // bitstream, and changing it changes all of them -- so say which, by name, rather than let
    // the operator discover it mid-stream.
    QStringList shared;
    for (const ldoc::Output& o : p.outputs) {
        if (o.encode == out->encode && o.id != out->id)
            shared.append(qstr(o.name));
    }

    ldoc::Encode next = *existing;
    change(next);
    retitle_if_auto(p, *existing, next);
    if (next == *existing)
        return;
    run(ldoc::SetEncode{std::move(next)});
    if (!shared.isEmpty())
        emit notice(QStringLiteral("'%1' shares this encode with %2, which changed too.")
                        .arg(qstr(out->name), shared.join(QStringLiteral(", "))));
}

void Document::setOutputBitrate(const QString& id, int kbps) {
    edit_encode_of(id, [kbps](ldoc::Encode& e) { e.encoder.bitrate_kbps = kbps; });
}

void Document::setOutputEncoder(const QString& id, const QString& codec,
                                const QString& implementation) {
    edit_encode_of(id, [&](ldoc::Encode& e) {
        e.encoder.codec = to_std(codec);
        e.encoder.implementation = to_std(implementation);
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

// ---- the nodes of the current scene -----------------------------------------------------------

namespace {

/// Where a new node goes: inside the selected group if one is selected, otherwise at the top of
/// the scene. Top, because `roots[0]` is the front-most node and a new thing should be visible.
struct Destination {
    std::optional<ldoc::Id> parent;
    std::size_t index = 0;
};

} // namespace

void Document::addSourceNode(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Source,
                                   name.isEmpty() ? std::string{"Source"} : to_std(name));
    const QString id = id_str(n.id);

    Destination dest;
    if (const ldoc::Node* sel = current_node(); sel && sel->is_group())
        dest.parent = sel->id;
    run(ldoc::AddNode{s->id, dest.parent, dest.index, std::move(n)});
    selectNode(id);
}

void Document::addGroup(const QString& name) {
    const ldoc::Scene* s = current_scene();
    if (!s)
        return;
    ldoc::Node n = ldoc::make_node(ldoc::NodeKind::Group,
                                   name.isEmpty() ? std::string{"Group"} : to_std(name));
    const QString id = id_str(n.id);
    run(ldoc::AddNode{s->id, std::nullopt, 0, std::move(n)});
    selectNode(id);
}

void Document::groupSelectedNodes() {
    const ldoc::Scene* s = current_scene();
    const QStringList chosen = selectedNodes();
    if (!s || chosen.isEmpty()) {
        emit notice(QStringLiteral("Select the sources to group first."));
        return;
    }

    // Where the group goes: the place of the topmost selected node, so the grouped items keep
    // their draw order relative to everything else (ADR-0037). selectedNodes() is already in
    // scene order, so the first entry is that node.
    struct At {
        std::optional<ldoc::Id> parent;
        std::size_t index = 0;
        bool found = false;
    };
    const auto place_of = [&](const ldoc::Id& wanted) {
        At at;
        const std::function<void(const std::vector<ldoc::Node>&, std::optional<ldoc::Id>)> walk =
            [&](const std::vector<ldoc::Node>& nodes, std::optional<ldoc::Id> parent) {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    if (nodes[i].id == wanted) {
                        at = At{parent, i, true};
                        return;
                    }
                    if (const auto* g = std::get_if<ldoc::GroupData>(&nodes[i].data))
                        walk(g->children, nodes[i].id);
                }
            };
        walk(s->roots, std::nullopt);
        return at;
    };

    const auto first = parse_id(chosen.front());
    if (!first)
        return;
    const At top = place_of(*first);
    if (!top.found)
        return;

    // A node cannot be moved inside a group that is itself being moved inside the new one, and
    // a group that contains another selected node would take it along twice. Dropping the
    // descendants is right rather than refusing: selecting a group and something inside it
    // plainly means "group these", and the inner one is already coming.
    QStringList moving;
    for (const QString& id : chosen) {
        const auto parsed = parse_id(id);
        if (!parsed)
            continue;
        bool inside_another = false;
        for (const QString& other : chosen) {
            if (other == id)
                continue;
            const auto other_id = parse_id(other);
            const ldoc::Node* o = other_id ? ldoc::find_node(s->roots, *other_id) : nullptr;
            if (!o)
                continue;
            ldoc::visit(*o, [&](const ldoc::Node& n) {
                if (n.id == *parsed && o->id != *parsed)
                    inside_another = true;
            });
        }
        if (!inside_another)
            moving << id;
    }
    if (moving.isEmpty())
        return;

    ldoc::Node group = ldoc::make_node(ldoc::NodeKind::Group, "Group");
    const ldoc::Id group_id = group.id;

    std::vector<ldoc::Command> steps;
    steps.push_back(ldoc::AddNode{s->id, top.parent, top.index, std::move(group)});
    // In order, each to the end of the group, so the list inside reads the way it read outside.
    std::size_t at = 0;
    for (const QString& id : moving) {
        const auto parsed = parse_id(id);
        if (!parsed)
            continue;
        steps.push_back(ldoc::MoveNode{s->id, *parsed, group_id, at});
        ++at;
    }
    run(ldoc::make_batch(std::move(steps), "Group sources"));
    selectNode(id_str(group_id));
}

QString Document::parentOfNode(const QString& nodeId) const {
    const ldoc::Scene* s = current_scene();
    const auto wanted = parse_id(nodeId);
    if (!s || !wanted)
        return {};
    QString found;
    const std::function<void(const std::vector<ldoc::Node>&, const QString&)> walk =
        [&](const std::vector<ldoc::Node>& nodes, const QString& parent) {
            for (const ldoc::Node& n : nodes) {
                if (n.id == *wanted) {
                    found = parent;
                    return;
                }
                if (const auto* g = std::get_if<ldoc::GroupData>(&n.data))
                    walk(g->children, id_str(n.id));
            }
        };
    walk(s->roots, QString{});
    return found;
}

void Document::removeNode(const QString& id) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RemoveNode{s->id, *parsed});
}

void Document::renameNode(const QString& id, const QString& name) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::RenameNode{s->id, *parsed, to_std(name)});
}

void Document::setNodeVisible(const QString& id, bool visible) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeVisible{s->id, *parsed, visible});
}

void Document::setNodeLocked(const QString& id, bool locked) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (s && parsed)
        run(ldoc::SetNodeLocked{s->id, *parsed, locked});
}

void Document::moveNode(const QString& id, int delta) {
    const ldoc::Scene* s = current_scene();
    const auto parsed = parse_id(id);
    if (!s || !parsed || delta == 0)
        return;

    // Find the node's siblings so the move stays within its own level: the Sources panel's up and
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

void Document::setNodePosition(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.position = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeScale(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.scale = ldoc::Vec2{x, y};
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeRotation(double degrees) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Transform t = n->transform;
    t.rotation_deg = degrees;
    run(ldoc::SetNodeTransform{s->id, n->id, t});
}

void Document::setNodeOpacity(double opacity) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
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

void Document::setNodeFit(const QString& fit) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    const auto parsed = ldoc::fit_mode_from_string(to_std(fit));
    if (!s || !n || !parsed)
        return;
    ldoc::Layout l = n->layout;
    l.fit = *parsed;
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeBoxSize(double w, double h) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.size = ldoc::Vec2{std::max(0.0, w), std::max(0.0, h)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeCrop(double left, double top, double right, double bottom) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.crop = ldoc::CropRect{std::max(0.0, left), std::max(0.0, top), std::max(0.0, right),
                            std::max(0.0, bottom)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeAlignment(double x, double y) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
    if (!s || !n)
        return;
    ldoc::Layout l = n->layout;
    l.alignment = ldoc::Vec2{std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
    run(ldoc::SetNodeLayout{s->id, n->id, l});
}

void Document::setNodeBlendMode(const QString& mode) {
    const ldoc::Scene* s = current_scene();
    const ldoc::Node* n = current_node();
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
