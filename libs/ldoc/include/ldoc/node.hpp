// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/types.hpp"

#include <functional>
#include <optional>
#include <variant>
#include <vector>

namespace ldoc {

/// One entry in a node's effect stack.
///
/// The `type` is resolved against a manifest in `schema/effects/*.toml`, not against a compiled-in
/// enum -- that is what lets an effect ship as shader text plus a parameter schema rather than as
/// native code loaded into the render process (ADR-0004).
struct Effect {
    Id id;
    std::string type;
    bool enabled = true;
    ParamMap params;
    /// A Mask node that gates this effect. Points anywhere in the scene.
    std::optional<Id> mask;
    /// How this effect's output combines with its input. Absent means "replace".
    std::optional<Blend> blend;

    friend bool operator==(const Effect&, const Effect&) = default;
};

/// A gain-staged route from one bus to another.
struct Send {
    Id to;
    double gain_db = 0.0;
    bool pre_fader = false;

    friend bool operator==(const Send&, const Send&) = default;
};

struct AudioRouting {
    /// Absent means the layer's audio is not routed anywhere -- silent, not "default bus".
    /// Defaulting would make a muted source and an unrouted source indistinguishable.
    std::optional<Id> bus;
    std::vector<Send> sends;
    double gain_db = 0.0;
    bool mute = false;

    friend bool operator==(const AudioRouting&, const AudioRouting&) = default;
};

enum class NodeKind : std::uint8_t {
    /// A real container with its own transform, opacity, blend and effects -- not OBS's
    /// presentation-only grouping.
    Group,
    /// Binds a Source instance.
    Layer,
    /// Effects only, applied to the composite beneath it. What makes blend modes genuinely
    /// useful, and what OBS has no equivalent of.
    Adjustment,
    /// Gates what is beneath it by alpha, luma or a shape.
    Mask,
    /// Shows another node. Source-clone as a document concept rather than a plugin.
    Reference,
    /// A whole scene rendered offscreen at its own resolution and colour space.
    Precomp,
};

[[nodiscard]] std::string_view to_string(NodeKind k);
[[nodiscard]] std::optional<NodeKind> node_kind_from_string(std::string_view s);

struct Node;

struct GroupData {
    /// **`children[0]` is the topmost layer**, drawn last.
    ///
    /// The order a layers panel shows, so what the operator reads top-to-bottom is what the list
    /// holds front-to-back. Choosing the other convention means every piece of UI reverses the
    /// list and one of them eventually forgets.
    std::vector<Node> children;
    /// Composite against transparency instead of the backdrop beneath the group.
    bool isolate = false;

    friend bool operator==(const GroupData&, const GroupData&) = default;
};

struct LayerData {
    /// The Source instance this layer shows. Sources live at project level, so two layers -- or
    /// two scenes -- can show one capture without opening the device twice.
    std::optional<Id> source;
    AudioRouting audio;

    friend bool operator==(const LayerData&, const LayerData&) = default;
};

struct AdjustmentData {
    friend bool operator==(const AdjustmentData&, const AdjustmentData&) = default;
};

enum class MaskKind : std::uint8_t { Alpha, Luma, Shape };

[[nodiscard]] std::string_view to_string(MaskKind k);
[[nodiscard]] std::optional<MaskKind> mask_kind_from_string(std::string_view s);

struct MaskData {
    MaskKind kind = MaskKind::Alpha;
    bool invert = false;
    /// Geometry for Shape masks. Held as parameters so a shape kind can be added without a
    /// format change.
    ParamMap shape;

    friend bool operator==(const MaskData&, const MaskData&) = default;
};

enum class ReferenceMode : std::uint8_t {
    /// Follows the target as it changes.
    Live,
    /// Holds the target as it was when the reference was made.
    Snapshot,
};

[[nodiscard]] std::string_view to_string(ReferenceMode m);
[[nodiscard]] std::optional<ReferenceMode> reference_mode_from_string(std::string_view s);

struct ReferenceData {
    Id target;
    ReferenceMode mode = ReferenceMode::Live;

    friend bool operator==(const ReferenceData&, const ReferenceData&) = default;
};

struct PrecompData {
    Id scene;
    /// (0, 0) means "the size of the canvas this precomp is being composited into".
    Vec2 size{};
    ColorSpace color_space = ColorSpace::Rec709;

    friend bool operator==(const PrecompData&, const PrecompData&) = default;
};

/// Per-canvas deltas for one node.
///
/// This is the whole reason a vertical feed is not a duplicated scene: a scene is
/// canvas-agnostic, and portrait is the same scene with a handful of nodes repositioned. Two
/// duplicated scenes drift the moment anything changes in one of them, which is the failure every
/// OBS vertical-streaming setup eventually hits.
struct CanvasOverride {
    std::optional<Transform> transform;
    std::optional<Layout> layout;
    std::optional<bool> visible;
    std::optional<Blend> blend;

    [[nodiscard]] bool empty() const {
        return !transform && !layout && !visible && !blend;
    }

    friend bool operator==(const CanvasOverride&, const CanvasOverride&) = default;
};

using NodeData = std::variant<GroupData, LayerData, AdjustmentData, MaskData, ReferenceData,
                              PrecompData>;

/// A node in a scene tree.
///
/// Kind-specific fields live in a variant rather than side by side in one flat struct, so a Layer
/// cannot carry children and a Reference cannot carry a source. States that mean nothing should
/// not be representable; a validator that has to reject them is a validator that will be run too
/// late somewhere.
struct Node {
    Id id;
    std::string name;
    bool visible = true;
    bool locked = false;
    Transform transform{};
    Layout layout{};
    Blend blend{};
    std::vector<Effect> effects;
    std::map<Id, CanvasOverride> canvas_overrides;
    NodeData data = GroupData{};

    [[nodiscard]] NodeKind kind() const;
    [[nodiscard]] bool is_group() const { return std::holds_alternative<GroupData>(data); }

    /// The children of a Group, or nullptr for every other kind.
    [[nodiscard]] std::vector<Node>* children();
    [[nodiscard]] const std::vector<Node>* children() const;

    friend bool operator==(const Node&, const Node&) = default;
};

/// Build a node of a given kind with a fresh id.
[[nodiscard]] Node make_node(NodeKind kind, std::string name);

/// Depth-first, parents before children, children in list order (so topmost first).
void visit(Node& root, const std::function<void(Node&)>& fn);
void visit(const Node& root, const std::function<void(const Node&)>& fn);
void visit(std::vector<Node>& roots, const std::function<void(Node&)>& fn);
void visit(const std::vector<Node>& roots, const std::function<void(const Node&)>& fn);

[[nodiscard]] Node* find_node(std::vector<Node>& roots, const Id& id);
[[nodiscard]] const Node* find_node(const std::vector<Node>& roots, const Id& id);

/// Deep-copy a subtree, minting a fresh id for every node and every effect in it.
///
/// References that point *within* the copied subtree are rewritten through the remap table;
/// references that point outside it are preserved exactly. Both halves matter: duplicating a
/// group that contains a Reference to one of its own members should give a group referring to the
/// *copy*, while duplicating a group whose Reference points at something elsewhere in the scene
/// should still point at that same thing.
///
/// A Layer's `source` is deliberately **not** remapped. Sources are project-level instances, and
/// duplicating a layer that shows a camera should show that camera, not open a second one.
/// Likewise a Precomp's `scene` and the canvas ids in `canvas_overrides`.
///
/// Preserving ids across a copy guarantees collisions and is a correctness bug, not a
/// convenience.
[[nodiscard]] Node duplicate(const Node& node, std::map<Id, Id>* remap_out = nullptr);
[[nodiscard]] std::vector<Node> duplicate(const std::vector<Node>& nodes,
                                          std::map<Id, Id>* remap_out = nullptr);

/// Every id this subtree *defines* (its nodes and their effects).
void collect_defined_ids(const std::vector<Node>& roots, std::vector<Id>& out);
/// Every id this subtree *refers to*, paired with what kind of thing it should resolve to.
enum class RefKind : std::uint8_t { Node, Source, Scene, Canvas, Bus };
struct Reference {
    Id id;
    RefKind kind = RefKind::Node;
    /// The node the reference was found on, for error messages that name something findable.
    Id from;
};
void collect_references(const std::vector<Node>& roots, std::vector<Reference>& out);

} // namespace ldoc
