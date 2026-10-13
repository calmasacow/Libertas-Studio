// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

/// The command bus.
///
/// Every document mutation is one of these: validated, serialisable, journalable and undoable.
/// The UI, global hotkeys, a Stream Deck, a websocket API and scripting are then all the same
/// thing -- producers of commands -- rather than five code paths that each mutate the document
/// slightly differently and disagree about undo.
///
/// Two properties make the rest of the system fall out of this one:
///
/// **Applying a command returns its inverse, and the inverse is itself a command.** Undo is
/// therefore not a separate mechanism with its own bugs; it is the same apply() running a command
/// that was built from the state the first one replaced. It also means an undo step can be
/// written to the journal exactly like anything else.
///
/// **A command that cannot be applied changes nothing.** Every one validates before it mutates,
/// so a refused command leaves the project byte-identical rather than half-applied.
namespace ldoc {

// ---- project --------------------------------------------------------------------------------

struct SetProjectName {
    std::string name;
    friend bool operator==(const SetProjectName&, const SetProjectName&) = default;
};

// ---- scenes ---------------------------------------------------------------------------------

struct AddScene {
    Scene scene;
    std::size_t index = 0;
    friend bool operator==(const AddScene&, const AddScene&) = default;
};

struct RemoveScene {
    Id scene;
    friend bool operator==(const RemoveScene&, const RemoveScene&) = default;
};

struct RenameScene {
    Id scene;
    std::string name;
    friend bool operator==(const RenameScene&, const RenameScene&) = default;
};

// ---- nodes ----------------------------------------------------------------------------------

struct AddNode {
    Id scene;
    /// Absent means a root of the scene.
    std::optional<Id> parent;
    std::size_t index = 0;
    Node node;
    friend bool operator==(const AddNode&, const AddNode&) = default;
};

struct RemoveNode {
    Id scene;
    Id node;
    friend bool operator==(const RemoveNode&, const RemoveNode&) = default;
};

struct MoveNode {
    Id scene;
    Id node;
    std::optional<Id> parent;
    std::size_t index = 0;
    friend bool operator==(const MoveNode&, const MoveNode&) = default;
};

struct RenameNode {
    Id scene;
    Id node;
    std::string name;
    friend bool operator==(const RenameNode&, const RenameNode&) = default;
};

struct SetNodeVisible {
    Id scene;
    Id node;
    bool visible = true;
    friend bool operator==(const SetNodeVisible&, const SetNodeVisible&) = default;
};

struct SetNodeLocked {
    Id scene;
    Id node;
    bool locked = false;
    friend bool operator==(const SetNodeLocked&, const SetNodeLocked&) = default;
};

struct SetNodeTransform {
    Id scene;
    Id node;
    Transform transform;
    friend bool operator==(const SetNodeTransform&, const SetNodeTransform&) = default;
};

struct SetNodeLayout {
    Id scene;
    Id node;
    Layout layout;
    friend bool operator==(const SetNodeLayout&, const SetNodeLayout&) = default;
};

struct SetNodeBlend {
    Id scene;
    Id node;
    Blend blend;
    friend bool operator==(const SetNodeBlend&, const SetNodeBlend&) = default;
};

/// Absent `value` removes the override, which is how a node goes back to following the scene on
/// that canvas.
struct SetCanvasOverride {
    Id scene;
    Id node;
    Id canvas;
    std::optional<CanvasOverride> value;
    friend bool operator==(const SetCanvasOverride&, const SetCanvasOverride&) = default;
};

// ---- effects --------------------------------------------------------------------------------

struct AddEffect {
    Id scene;
    Id node;
    std::size_t index = 0;
    Effect effect;
    friend bool operator==(const AddEffect&, const AddEffect&) = default;
};

struct RemoveEffect {
    Id scene;
    Id node;
    Id effect;
    friend bool operator==(const RemoveEffect&, const RemoveEffect&) = default;
};

struct SetEffectParams {
    Id scene;
    Id node;
    Id effect;
    ParamMap params;
    friend bool operator==(const SetEffectParams&, const SetEffectParams&) = default;
};

struct SetEffectEnabled {
    Id scene;
    Id node;
    Id effect;
    bool enabled = true;
    friend bool operator==(const SetEffectEnabled&, const SetEffectEnabled&) = default;
};

// ---- decks ----------------------------------------------------------------------------------

struct SetDeckProgram {
    Id deck;
    std::optional<Id> scene;
    friend bool operator==(const SetDeckProgram&, const SetDeckProgram&) = default;
};

struct SetDeckPreview {
    Id deck;
    std::optional<Id> scene;
    friend bool operator==(const SetDeckPreview&, const SetDeckPreview&) = default;
};

/// Absent clears it, so the canvas falls back to black and silence.
///
/// This permits any scene id, including one that does not exist or is unsuitable: see ADR-0023 on
/// why a command may only refuse states the document model itself refuses. Checking suitability,
/// explaining it and offering to fix the clip's loop setting is the designation flow's job, which
/// runs `check_emergency_scene` before issuing this at all.
struct SetCanvasEmergencyScene {
    Id canvas;
    std::optional<Id> scene;
    friend bool operator==(const SetCanvasEmergencyScene&,
                           const SetCanvasEmergencyScene&) = default;
};

using Command =
    std::variant<SetProjectName, AddScene, RemoveScene, RenameScene, AddNode, RemoveNode,
                 MoveNode, RenameNode, SetNodeVisible, SetNodeLocked, SetNodeTransform,
                 SetNodeLayout, SetNodeBlend, SetCanvasOverride, AddEffect, RemoveEffect,
                 SetEffectParams, SetEffectEnabled, SetDeckProgram, SetDeckPreview,
                 SetCanvasEmergencyScene>;

struct Applied {
    /// A command that, applied to the resulting project, restores the previous one.
    Command inverse;
    /// What to put in the undo menu: "Hide Camera", "Rename scene to 'Intro'".
    std::string description;
};

/// Apply `command` to `project`.
///
/// On failure the project is untouched and the error says what was wrong, in terms of the
/// document rather than of the code.
[[nodiscard]] std::expected<Applied, std::string> apply(Project& project,
                                                        const Command& command);

/// The stable wire name, as it appears in the journal and on the websocket API.
[[nodiscard]] std::string_view command_name(const Command& command);
[[nodiscard]] std::string describe(const Command& command);

[[nodiscard]] std::string command_to_json(const Command& command);
[[nodiscard]] std::expected<Command, std::string> command_from_json(std::string_view text);

} // namespace ldoc
