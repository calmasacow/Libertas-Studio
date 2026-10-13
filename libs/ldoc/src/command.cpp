// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/command.hpp"

#include <algorithm>
#include <format>
#include <unordered_set>

namespace ldoc {
namespace {

/// A node together with the list it lives in, so it can be removed, reinserted or moved.
struct Located {
    std::vector<Node>* siblings = nullptr;
    std::size_t index = 0;
    std::optional<Id> parent;
};

std::optional<Located> locate(std::vector<Node>& roots, const Id& wanted,
                              std::optional<Id> parent = std::nullopt) {
    for (std::size_t i = 0; i < roots.size(); ++i) {
        if (roots[i].id == wanted)
            return Located{&roots, i, parent};
        if (auto* kids = roots[i].children()) {
            if (auto found = locate(*kids, wanted, roots[i].id))
                return found;
        }
    }
    return std::nullopt;
}

std::expected<Scene*, std::string> find_scene(Project& p, const Id& id) {
    Scene* s = p.scene(id);
    if (!s)
        return std::unexpected(std::format("there is no scene {}", id.to_string()));
    return s;
}

std::expected<Node*, std::string> find_in_scene(Scene& s, const Id& id) {
    Node* n = find_node(s.roots, id);
    if (!n)
        return std::unexpected(
            std::format("there is no node {} in scene '{}'", id.to_string(), s.name));
    return n;
}

std::expected<Effect*, std::string> find_effect(Node& n, const Id& id) {
    const auto it = std::find_if(n.effects.begin(), n.effects.end(),
                                 [&id](const Effect& e) { return e.id == id; });
    if (it == n.effects.end())
        return std::unexpected(
            std::format("there is no effect {} on '{}'", id.to_string(), n.name));
    return &*it;
}

std::expected<Deck*, std::string> find_deck(Project& p, const Id& id) {
    const auto it = std::find_if(p.decks.begin(), p.decks.end(),
                                 [&id](const Deck& d) { return d.id == id; });
    if (it == p.decks.end())
        return std::unexpected(std::format("there is no deck {}", id.to_string()));
    return &*it;
}

/// Every id the project already defines. Used to refuse an insert that would create a duplicate,
/// which is a corruption the validator would catch later but the command should never create.
std::unordered_set<Id> all_ids(const Project& p) {
    std::unordered_set<Id> ids;
    ids.insert(p.id);
    for (const Canvas& c : p.canvases)
        ids.insert(c.id);
    for (const Bus& b : p.buses) {
        ids.insert(b.id);
        for (const Effect& e : b.inserts)
            ids.insert(e.id);
    }
    for (const Source& s : p.sources)
        ids.insert(s.id);
    for (const Asset& a : p.assets)
        ids.insert(a.id);
    for (const Deck& d : p.decks)
        ids.insert(d.id);
    for (const Output& o : p.outputs)
        ids.insert(o.id);
    for (const Scene& s : p.scenes) {
        ids.insert(s.id);
        visit(s.roots, [&ids](const Node& n) {
            ids.insert(n.id);
            for (const Effect& e : n.effects)
                ids.insert(e.id);
        });
    }
    return ids;
}

/// True when `candidate` is `root` or lives beneath it.
bool is_within(const Node& root, const Id& candidate) {
    bool found = false;
    visit(root, [&](const Node& n) { found = found || n.id == candidate; });
    return found;
}

std::string quoted(const std::string& s) {
    return s.empty() ? std::string{"(unnamed)"} : std::format("'{}'", s);
}

const char* node_label(const Project& p, const Id& scene, const Id& node) {
    const Scene* s = p.scene(scene);
    if (!s)
        return "node";
    const Node* n = find_node(s->roots, node);
    return (n && !n->name.empty()) ? n->name.c_str() : "node";
}

} // namespace

std::string_view command_name(const Command& command) {
    return std::visit(
        [](const auto& c) -> std::string_view {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetProjectName>) return "project.set-name";
            else if constexpr (std::is_same_v<T, AddScene>) return "scene.add";
            else if constexpr (std::is_same_v<T, RemoveScene>) return "scene.remove";
            else if constexpr (std::is_same_v<T, RenameScene>) return "scene.rename";
            else if constexpr (std::is_same_v<T, AddNode>) return "node.add";
            else if constexpr (std::is_same_v<T, RemoveNode>) return "node.remove";
            else if constexpr (std::is_same_v<T, MoveNode>) return "node.move";
            else if constexpr (std::is_same_v<T, RenameNode>) return "node.rename";
            else if constexpr (std::is_same_v<T, SetNodeVisible>) return "node.set-visible";
            else if constexpr (std::is_same_v<T, SetNodeLocked>) return "node.set-locked";
            else if constexpr (std::is_same_v<T, SetNodeTransform>) return "node.set-transform";
            else if constexpr (std::is_same_v<T, SetNodeLayout>) return "node.set-layout";
            else if constexpr (std::is_same_v<T, SetNodeBlend>) return "node.set-blend";
            else if constexpr (std::is_same_v<T, SetCanvasOverride>)
                return "node.set-canvas-override";
            else if constexpr (std::is_same_v<T, AddEffect>) return "effect.add";
            else if constexpr (std::is_same_v<T, RemoveEffect>) return "effect.remove";
            else if constexpr (std::is_same_v<T, SetEffectParams>) return "effect.set-params";
            else if constexpr (std::is_same_v<T, SetEffectEnabled>) return "effect.set-enabled";
            else if constexpr (std::is_same_v<T, SetDeckProgram>) return "deck.set-program";
            else if constexpr (std::is_same_v<T, SetDeckPreview>) return "deck.set-preview";
            else if constexpr (std::is_same_v<T, SetCanvasEmergencyScene>)
                return "canvas.set-emergency-scene";
            else if constexpr (std::is_same_v<T, SetLayerSource>) return "layer.set-source";
            else if constexpr (std::is_same_v<T, AddSource>) return "source.add";
            else if constexpr (std::is_same_v<T, RemoveSource>) return "source.remove";
            else if constexpr (std::is_same_v<T, SetSource>) return "source.set";
            else if constexpr (std::is_same_v<T, AddCanvas>) return "canvas.add";
            else if constexpr (std::is_same_v<T, RemoveCanvas>) return "canvas.remove";
            else return "canvas.set";
        },
        command);
}

std::string describe(const Command& command) {
    return std::visit(
        [](const auto& c) -> std::string {
            using T = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<T, SetProjectName>)
                return std::format("Rename project to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, AddScene>)
                return std::format("Add scene {}", quoted(c.scene.name));
            else if constexpr (std::is_same_v<T, RemoveScene>)
                return "Remove scene";
            else if constexpr (std::is_same_v<T, RenameScene>)
                return std::format("Rename scene to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, AddNode>)
                return std::format("Add {}", quoted(c.node.name));
            else if constexpr (std::is_same_v<T, RemoveNode>)
                return "Remove layer";
            else if constexpr (std::is_same_v<T, MoveNode>)
                return "Move layer";
            else if constexpr (std::is_same_v<T, RenameNode>)
                return std::format("Rename layer to {}", quoted(c.name));
            else if constexpr (std::is_same_v<T, SetNodeVisible>)
                return c.visible ? "Show layer" : "Hide layer";
            else if constexpr (std::is_same_v<T, SetNodeLocked>)
                return c.locked ? "Lock layer" : "Unlock layer";
            else if constexpr (std::is_same_v<T, SetNodeTransform>)
                return "Transform layer";
            else if constexpr (std::is_same_v<T, SetNodeLayout>)
                return "Change layer fit";
            else if constexpr (std::is_same_v<T, SetNodeBlend>)
                return "Change blending";
            else if constexpr (std::is_same_v<T, SetCanvasOverride>)
                return c.value ? "Set per-canvas override" : "Clear per-canvas override";
            else if constexpr (std::is_same_v<T, AddEffect>)
                return std::format("Add effect {}", quoted(c.effect.type));
            else if constexpr (std::is_same_v<T, RemoveEffect>)
                return "Remove effect";
            else if constexpr (std::is_same_v<T, SetEffectParams>)
                return "Adjust effect";
            else if constexpr (std::is_same_v<T, SetEffectEnabled>)
                return c.enabled ? "Enable effect" : "Disable effect";
            else if constexpr (std::is_same_v<T, SetDeckProgram>)
                return c.scene ? "Switch program scene" : "Clear program scene";
            else if constexpr (std::is_same_v<T, SetDeckPreview>)
                return c.scene ? "Set preview scene" : "Clear preview scene";
            else if constexpr (std::is_same_v<T, SetCanvasEmergencyScene>)
                return c.scene ? "Set emergency scene" : "Clear emergency scene";
            else if constexpr (std::is_same_v<T, SetLayerSource>)
                return c.source ? "Bind layer to a source" : "Unbind layer";
            else if constexpr (std::is_same_v<T, AddSource>)
                return std::format("Add source {}", quoted(c.source.name));
            else if constexpr (std::is_same_v<T, RemoveSource>)
                return "Remove source";
            else if constexpr (std::is_same_v<T, SetSource>)
                return std::format("Change source {}", quoted(c.value.name));
            else if constexpr (std::is_same_v<T, AddCanvas>)
                return std::format("Add canvas {}", quoted(c.canvas.name));
            else if constexpr (std::is_same_v<T, RemoveCanvas>)
                return "Remove canvas";
            else
                return std::format("Change canvas {}", quoted(c.value.name));
        },
        command);
}

namespace {

/// One overload per command. Each validates completely before touching the project, and returns
/// the command that puts things back.
/// A command validates its *preconditions* -- the things it acts on must exist -- but not its
/// *references*.
///
/// The distinction is forced by undo. Setting a deck's program scene captures the previous one
/// as its inverse; if that scene is later deleted, the inverse now points at something gone. Undo
/// has to be able to restore it anyway, because that is the state the command replaced. A command
/// may therefore only refuse states the document model itself refuses -- and a dangling reference
/// is a warning there, not an error, precisely so that deleting things stays possible.
///
/// Keeping the operator from *choosing* a scene that does not exist is the UI's job: it offers
/// the ones that do.
struct Apply {
    Project& p;

    std::expected<Applied, std::string> operator()(const SetProjectName& c) const {
        Applied out{SetProjectName{p.name}, describe(c)};
        p.name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddScene& c) const {
        if (c.index > p.scenes.size())
            return std::unexpected(std::format("scene position {} is past the end", c.index));
        const auto existing = all_ids(p);
        std::vector<Id> incoming{c.scene.id};
        collect_defined_ids(c.scene.roots, incoming);
        for (const Id& id : incoming) {
            if (id.is_nil())
                return std::unexpected("the scene contains something with no id");
            if (existing.contains(id))
                return std::unexpected(
                    std::format("id {} is already in the project", id.to_string()));
        }
        Applied out{RemoveScene{c.scene.id}, describe(c)};
        p.scenes.insert(p.scenes.begin() + static_cast<std::ptrdiff_t>(c.index), c.scene);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveScene& c) const {
        const auto it = std::find_if(p.scenes.begin(), p.scenes.end(),
                                     [&c](const Scene& s) { return s.id == c.scene; });
        if (it == p.scenes.end())
            return std::unexpected(std::format("there is no scene {}", c.scene.to_string()));
        const auto index = static_cast<std::size_t>(it - p.scenes.begin());
        Applied out{AddScene{*it, index}, std::format("Remove scene {}", quoted(it->name))};
        p.scenes.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RenameScene& c) const {
        auto s = find_scene(p, c.scene);
        if (!s)
            return std::unexpected(s.error());
        Applied out{RenameScene{c.scene, (*s)->name}, describe(c)};
        (*s)->name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());

        const auto existing = all_ids(p);
        std::vector<Id> incoming;
        collect_defined_ids({c.node}, incoming);
        for (const Id& id : incoming) {
            if (id.is_nil())
                return std::unexpected("the node, or something on it, has no id");
            if (existing.contains(id))
                return std::unexpected(
                    std::format("id {} is already in the project", id.to_string()));
        }

        std::vector<Node>* target = &(*scene)->roots;
        if (c.parent) {
            Node* parent = find_node((*scene)->roots, *c.parent);
            if (!parent)
                return std::unexpected(
                    std::format("there is no node {} to add to", c.parent->to_string()));
            target = parent->children();
            if (!target)
                return std::unexpected(
                    std::format("{} is not a group, so nothing can be added to it",
                                quoted(parent->name)));
        }
        if (c.index > target->size())
            return std::unexpected(std::format("position {} is past the end", c.index));

        Applied out{RemoveNode{c.scene, c.node.id}, describe(c)};
        target->insert(target->begin() + static_cast<std::ptrdiff_t>(c.index), c.node);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto at = locate((*scene)->roots, c.node);
        if (!at)
            return std::unexpected(std::format("there is no node {} in scene {}",
                                               c.node.to_string(), quoted((*scene)->name)));
        Node removed = (*at->siblings)[at->index];
        Applied out{AddNode{c.scene, at->parent, at->index, removed},
                    std::format("Remove {}", quoted(removed.name))};
        at->siblings->erase(at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index));
        return out;
    }

    std::expected<Applied, std::string> operator()(const MoveNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto at = locate((*scene)->roots, c.node);
        if (!at)
            return std::unexpected(std::format("there is no node {} in scene {}",
                                               c.node.to_string(), quoted((*scene)->name)));

        // Moving a group into itself would detach the whole subtree from the scene and leave the
        // tree referring to nodes nothing owns. Refuse it here rather than discover it later.
        if (c.parent) {
            const Node& moving = (*at->siblings)[at->index];
            if (*c.parent == c.node || is_within(moving, *c.parent))
                return std::unexpected(
                    std::format("{} cannot be moved inside itself", quoted(moving.name)));
        }

        const Applied out{MoveNode{c.scene, c.node, at->parent, at->index},
                          std::format("Move {}",
                                      quoted((*at->siblings)[at->index].name))};

        Node moved = std::move((*at->siblings)[at->index]);
        at->siblings->erase(at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index));

        std::vector<Node>* target = &(*scene)->roots;
        if (c.parent) {
            Node* parent = find_node((*scene)->roots, *c.parent);
            if (!parent || !parent->children()) {
                // Put it back; nothing may be half-applied.
                at->siblings->insert(
                    at->siblings->begin() + static_cast<std::ptrdiff_t>(at->index),
                    std::move(moved));
                return std::unexpected(
                    std::format("there is no group {} to move into", c.parent->to_string()));
            }
            target = parent->children();
        }
        const std::size_t index = std::min(c.index, target->size());
        target->insert(target->begin() + static_cast<std::ptrdiff_t>(index), std::move(moved));
        return out;
    }

    std::expected<Applied, std::string> operator()(const RenameNode& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{RenameNode{c.scene, c.node, (*n)->name}, describe(c)};
        (*n)->name = c.name;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeVisible& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeVisible{c.scene, c.node, (*n)->visible},
                    std::format("{} {}", c.visible ? "Show" : "Hide",
                                quoted(node_label(p, c.scene, c.node)))};
        (*n)->visible = c.visible;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeLocked& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeLocked{c.scene, c.node, (*n)->locked}, describe(c)};
        (*n)->locked = c.locked;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeTransform& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeTransform{c.scene, c.node, (*n)->transform}, describe(c)};
        (*n)->transform = c.transform;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeLayout& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        Applied out{SetNodeLayout{c.scene, c.node, (*n)->layout}, describe(c)};
        (*n)->layout = c.layout;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetNodeBlend& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        if (c.blend.opacity < 0.0 || c.blend.opacity > 1.0)
            return std::unexpected(std::format("opacity {} is outside 0..1", c.blend.opacity));
        Applied out{SetNodeBlend{c.scene, c.node, (*n)->blend}, describe(c)};
        (*n)->blend = c.blend;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetCanvasOverride& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        // The canvas is a reference, not a precondition: see the note on Apply.

        const auto it = (*n)->canvas_overrides.find(c.canvas);
        std::optional<CanvasOverride> previous;
        if (it != (*n)->canvas_overrides.end())
            previous = it->second;

        Applied out{SetCanvasOverride{c.scene, c.node, c.canvas, previous}, describe(c)};
        if (c.value)
            (*n)->canvas_overrides.insert_or_assign(c.canvas, *c.value);
        else
            (*n)->canvas_overrides.erase(c.canvas);
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddEffect& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        if (c.effect.type.empty())
            return std::unexpected("an effect needs a type");
        if (c.effect.id.is_nil())
            return std::unexpected("an effect needs an id");
        if (all_ids(p).contains(c.effect.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.effect.id.to_string()));
        if (c.index > (*n)->effects.size())
            return std::unexpected(std::format("position {} is past the end", c.index));

        Applied out{RemoveEffect{c.scene, c.node, c.effect.id}, describe(c)};
        (*n)->effects.insert((*n)->effects.begin() + static_cast<std::ptrdiff_t>(c.index),
                             c.effect);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveEffect& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        const auto it = std::find_if((*n)->effects.begin(), (*n)->effects.end(),
                                     [&c](const Effect& e) { return e.id == c.effect; });
        if (it == (*n)->effects.end())
            return std::unexpected(std::format("there is no effect {} on {}",
                                               c.effect.to_string(), quoted((*n)->name)));
        const auto index = static_cast<std::size_t>(it - (*n)->effects.begin());
        Applied out{AddEffect{c.scene, c.node, index, *it},
                    std::format("Remove effect {}", quoted(it->type))};
        (*n)->effects.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEffectParams& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto e = find_effect(**n, c.effect);
        if (!e)
            return std::unexpected(e.error());
        Applied out{SetEffectParams{c.scene, c.node, c.effect, (*e)->params}, describe(c)};
        (*e)->params = c.params;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetEffectEnabled& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto e = find_effect(**n, c.effect);
        if (!e)
            return std::unexpected(e.error());
        Applied out{SetEffectEnabled{c.scene, c.node, c.effect, (*e)->enabled}, describe(c)};
        (*e)->enabled = c.enabled;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetLayerSource& c) const {
        auto scene = find_scene(p, c.scene);
        if (!scene)
            return std::unexpected(scene.error());
        auto n = find_in_scene(**scene, c.node);
        if (!n)
            return std::unexpected(n.error());
        auto* layer = std::get_if<LayerData>(&(*n)->data);
        if (!layer)
            return std::unexpected(
                std::format("{} is not a layer, so nothing can be bound to it",
                            quoted((*n)->name)));
        // The source is a reference, not a precondition: see the note on Apply.
        Applied out{SetLayerSource{c.scene, c.node, layer->source}, describe(c)};
        layer->source = c.source;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddSource& c) const {
        if (c.index > p.sources.size())
            return std::unexpected(std::format("source position {} is past the end", c.index));
        if (c.source.id.is_nil())
            return std::unexpected("a source needs an id");
        if (c.source.type.empty())
            return std::unexpected("a source needs a type");
        if (all_ids(p).contains(c.source.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.source.id.to_string()));
        Applied out{RemoveSource{c.source.id}, describe(c)};
        p.sources.insert(p.sources.begin() + static_cast<std::ptrdiff_t>(c.index), c.source);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveSource& c) const {
        const auto it = std::find_if(p.sources.begin(), p.sources.end(),
                                     [&c](const Source& s) { return s.id == c.source; });
        if (it == p.sources.end())
            return std::unexpected(std::format("there is no source {}", c.source.to_string()));
        const auto index = static_cast<std::size_t>(it - p.sources.begin());
        // Layers still bound to it are left pointing at nothing, which validate() reports as a
        // warning. Refusing the removal instead would mean a source could never be deleted while
        // anything used it -- see ADR-0023.
        Applied out{AddSource{*it, index}, std::format("Remove source {}", quoted(it->name))};
        p.sources.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetSource& c) const {
        const auto it = std::find_if(p.sources.begin(), p.sources.end(),
                                     [&c](const Source& s) { return s.id == c.value.id; });
        if (it == p.sources.end())
            return std::unexpected(
                std::format("there is no source {}", c.value.id.to_string()));
        if (c.value.type.empty())
            return std::unexpected("a source needs a type");
        Applied out{SetSource{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const AddCanvas& c) const {
        if (c.index > p.canvases.size())
            return std::unexpected(std::format("canvas position {} is past the end", c.index));
        if (c.canvas.id.is_nil())
            return std::unexpected("a canvas needs an id");
        if (c.canvas.width <= 0 || c.canvas.height <= 0)
            return std::unexpected(
                std::format("a canvas cannot be {}x{}", c.canvas.width, c.canvas.height));
        if (!c.canvas.fps.valid())
            return std::unexpected("a canvas needs a valid frame rate");
        if (all_ids(p).contains(c.canvas.id))
            return std::unexpected(
                std::format("id {} is already in the project", c.canvas.id.to_string()));
        Applied out{RemoveCanvas{c.canvas.id}, describe(c)};
        p.canvases.insert(p.canvases.begin() + static_cast<std::ptrdiff_t>(c.index), c.canvas);
        return out;
    }

    std::expected<Applied, std::string> operator()(const RemoveCanvas& c) const {
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.canvas; });
        if (it == p.canvases.end())
            return std::unexpected(std::format("there is no canvas {}", c.canvas.to_string()));
        // A deck or output without a canvas is an *error* rather than a warning -- it has no
        // output surface at all -- so this is a precondition and not a reference.
        for (const Deck& d : p.decks) {
            if (d.canvas == c.canvas)
                return std::unexpected(std::format(
                    "deck '{}' renders to this canvas; point it at another one first", d.name));
        }
        for (const Output& o : p.outputs) {
            if (o.canvas == c.canvas)
                return std::unexpected(std::format(
                    "output '{}' encodes this canvas; point it at another one first", o.name));
        }
        const auto index = static_cast<std::size_t>(it - p.canvases.begin());
        Applied out{AddCanvas{*it, index}, std::format("Remove canvas {}", quoted(it->name))};
        p.canvases.erase(it);
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetCanvas& c) const {
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.value.id; });
        if (it == p.canvases.end())
            return std::unexpected(
                std::format("there is no canvas {}", c.value.id.to_string()));
        if (c.value.width <= 0 || c.value.height <= 0)
            return std::unexpected(
                std::format("a canvas cannot be {}x{}", c.value.width, c.value.height));
        if (!c.value.fps.valid())
            return std::unexpected("a canvas needs a valid frame rate");
        Applied out{SetCanvas{*it}, describe(c)};
        *it = c.value;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetDeckProgram& c) const {
        auto d = find_deck(p, c.deck);
        if (!d)
            return std::unexpected(d.error());
        // The scene is a reference, not a precondition: see the note on Apply.
        Applied out{SetDeckProgram{c.deck, (*d)->program_scene}, describe(c)};
        (*d)->program_scene = c.scene;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetCanvasEmergencyScene& c) const {
        const auto it = std::find_if(p.canvases.begin(), p.canvases.end(),
                                     [&c](const Canvas& x) { return x.id == c.canvas; });
        if (it == p.canvases.end())
            return std::unexpected(std::format("there is no canvas {}", c.canvas.to_string()));
        // The scene is a reference, not a precondition: see the note on Apply.
        Applied out{SetCanvasEmergencyScene{c.canvas, it->emergency_scene}, describe(c)};
        it->emergency_scene = c.scene;
        return out;
    }

    std::expected<Applied, std::string> operator()(const SetDeckPreview& c) const {
        auto d = find_deck(p, c.deck);
        if (!d)
            return std::unexpected(d.error());
        Applied out{SetDeckPreview{c.deck, (*d)->preview_scene}, describe(c)};
        (*d)->preview_scene = c.scene;
        return out;
    }
};

} // namespace

std::expected<Applied, std::string> apply(Project& project, const Command& command) {
    return std::visit(Apply{project}, command);
}

} // namespace ldoc
