// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "document.hpp"

#include "ldoc/command.hpp"

#include <algorithm>
#include <functional>

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
    return m;
}

// ---- rebuilding the models -------------------------------------------------------------------

void Document::rebuild() {
    const ldoc::Project& p = session_->project();

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
        scenes.push_back(std::move(row));
    }
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

    // A selected thing can be deleted by an undo, so selection is validated on every change
    // rather than trusted.
    if (!selected_scene_.isEmpty() && !current_scene()) {
        selected_scene_ = p.scenes.empty() ? QString{} : id_str(p.scenes.front().id);
        selected_layer_.clear();
        emit selectionChanged();
    } else if (!selected_layer_.isEmpty() && !current_layer()) {
        selected_layer_.clear();
        emit selectionChanged();
    }

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

QVariantList Document::sourceTypes() const {
    struct Entry {
        const char* id;
        const char* title;
    };
    // Ordered so the ones that are safe in an emergency scene come first, since that is the
    // list an operator is reading when it matters most (ADR-0025).
    static constexpr Entry kTypes[] = {
        {"source.media", "Media File"},   {"source.image", "Image"},
        {"source.text", "Text"},          {"source.color", "Colour"},
        {"source.screen", "Screen"},      {"source.window", "Window"},
        {"source.v4l2", "Camera"},        {"source.browser", "Browser"},
        {"source.audio-input", "Audio Input"},
        {"source.audio-output", "Audio Output"},
    };
    QVariantList out;
    for (const Entry& e : kTypes) {
        QVariantMap m;
        m["id"] = QString::fromLatin1(e.id);
        m["title"] = QString::fromLatin1(e.title);
        m["selfContained"] = ldoc::is_self_contained(e.id);
        out.append(m);
    }
    return out;
}

void Document::addSource(const QString& name, const QString& type) {
    if (type.isEmpty())
        return;
    ldoc::Source src;
    src.id = ldoc::Id::generate();
    src.name = name.isEmpty() ? std::string{"Input"} : to_std(name);
    src.type = to_std(type);
    // A media source in an emergency scene has to loop or the net expires (ADR-0025), and
    // looping is the right default for every other use of one too.
    if (src.type == "source.media")
        src.settings.emplace("loop", true);
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
