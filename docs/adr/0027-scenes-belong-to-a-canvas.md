<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0027 — Scenes belong to a canvas, and a link is a cue rather than a copy

- **Status:** accepted
- **Date:** 2026-10-13
- **Depends on:** ADR-0020 (canvases are independent pipelines), ADR-0022 (the project is a
  directory), ADR-0023 (the command bus), ADR-0025 (the safe state)

## Context

A scene today is **project-level and canvas-agnostic**. `Project::scenes` is one flat list, and
what makes a scene look right on a portrait canvas lives in each node's `canvas_overrides` rather
than in a second scene. That claim was never argued in an ADR — it is a doc comment on `Scene` and
a paragraph in `docs/todo.md` — and the reasoning behind it was that two copies of a scene drift,
and the copy that drifts is always the one nobody is looking at.

That reasoning is sound about *copies*. It turned out not to be about *shows*.

The operator runs a widescreen programme and a vertical one at the same time. A vertical show is
not the widescreen one squashed; it is a different arrangement of the same cameras. Authoring it
as a pile of per-node overrides on the widescreen scene means authoring it **as a diff** — you
never see the vertical show, you see the places it disagrees with another show. The override map
also has to grow a key for every property anyone ever wants to differ, and every key is a second
code path at composite time.

Asked directly (2026-10-13), the operator settled it:

- Canvases are **completely independent**, and each owns its own scene list.
- **Nothing about a scene's contents ever crosses canvases.** Adding a camera, moving it, hiding
  it, deleting a scene, removing one source — none of it propagates.
- A link is not about content at all. It is about **what is on air**: when a scene is activated on
  the leading canvas and that scene has a link, the other canvas changes to the linked scene. With
  no link the other canvas does not move.
- Links are **one-way**. A link points from a scene on one canvas to a scene on another;
  activating the far side does nothing to the near side.
- A source used on both canvases is **one source, opened once**, as in OBS.

## Decision

### 1. A scene belongs to exactly one canvas

`Scene` gains `Id canvas`. `Project::scenes` stays a flat list rather than becoming
`Canvas::scenes`, because ADR-0022 stores one scene per file in `scenes/<ulid>.json` with a
manifest in `project.json`; nesting scenes inside canvases would either change that layout or
make the in-memory and on-disk shapes disagree about what a scene is. A flat list with an owner
field keeps both.

### 2. Nothing about contents is shared, so `canvas_overrides` goes

`Node::canvas_overrides` and `CanvasOverride` exist for exactly one purpose: making a single
shared scene look right on a second canvas. There are no shared scenes any more, so the field has
no purpose, and a field with no purpose is one that every later build must keep reading, writing
and honouring. It is deleted along with `SetNodeCanvasOverride`.

### 3. A link is a directed cue between two scenes on different canvases

```
struct SceneLink {
    Id id;
    Id from;   // a scene; activating it on its canvas's deck fires the cue
    Id to;     // a scene on a *different* canvas
};
```

They live in `Project::scene_links` rather than on either scene. A relationship is not a property
of either end, and deleting a scene has to prune every link that names it — one list is one place
to prune, and the linking panel is a view over that list rather than a walk of every scene.

When `from` becomes the program scene of its canvas's deck, the deck targeting `to`'s canvas takes
`to` to program using **that deck's own transition**, not the leading deck's. Two canvases are two
pipelines (ADR-0020) and a vertical show may well cut where the widescreen one dissolves.

Three rules keep it predictable:

- **Both ends must be on different canvases.** A link inside one canvas is a scene that activates
  itself.
- **At most one link from a given scene to a given canvas.** Two would be a cue with no answer to
  "which one". A scene driving *several* canvases is fine and is the three-canvas case.
- **One hop, never a chain.** If A drives B and B drives C, activating A takes B and stops. A
  chain is a cue sequence that no panel can show you, and with one-way links across three canvases
  it can close into a cycle. Anyone wanting C adds A → C, which is visible in the list.

### 4. Sources stay project-level and shared

`Project::sources` does not move. One camera, opened once, referenced from as many scenes and
canvases as you like. A per-canvas source list would mean either opening one device twice — which
fails outright — or a second list of references that must be kept in step with the first.

### 5. A cue can never take the emergency scene

`Canvas::emergency_scene` stays per canvas and is never the `to` of a link. The safe state is not
a cue; it is what happens when cues stop arriving (ADR-0025). A safety net that something else can
fire is not a safety net.

### 6. Main is the first canvas, and cannot be deleted or renamed

The cue model needs a leader, and the operator's answer is the one OBS gives: `canvases[0]` is
Main, it exists from the moment a project does, and nothing can remove or rename it.

"The first one in the list" is normally a bad rule, because reordering silently changes meaning.
It is safe here only because Main cannot be moved out of first place, deleted, or renamed — the
immutability is what makes the positional rule honest, so the two must ship together. `AddCanvas`
at index 0, `RemoveCanvas` on Main and `SetCanvasName` on Main are all refused by the command
layer, not merely hidden in the UI (ADR-0023: a command validates itself).

Links therefore always run **from a scene on Main to a scene on another canvas**. The chain icon
appears only on non-Main canvases, which is what makes the linking panel's direction
unambiguous without ever drawing an arrow.

### 7. Migration

`kSchemaVersion` goes to 2. A version 1 project has one flat scene list and may carry
`canvas_overrides`:

- Every scene is assigned the **first canvas**, which is the only one the shell has ever worked
  against.
- `canvas_overrides` are dropped. If any are present the loader **reports how many it dropped and
  on which scenes**, through the same channel that reports a quarantined scene file (ADR-0022).
  Silently discarding authored work is how a format bump earns a reputation.

## Rejected alternatives

**Keep scenes canvas-agnostic and grow the override map.** Today's model. Rejected because it makes
authoring a vertical show an exercise in editing a diff, and because the map must gain a key for
every property anyone ever wants to differ — each one a branch at composite time and a case in
every serialiser. The drift argument it was built on only applies to copies that are *meant* to be
the same thing; two shows are not.

**Full duplication with content synchronisation.** What `docs/todo.md` guessed the answer would be:
a link with a declared scope, where adding a layer follows but moving it does not. Rejected by the
operator, and rightly — it requires answering "which properties follow" for every property, and
every answer is wrong for somebody. It also puts a second author on your scene: a vertical
operator's edit changes the widescreen show.

**Two-way links.** Rejected: a vertical operator could change what the main programme is
broadcasting by touching their own canvas. One canvas leads and it is the show.

**Links as a field on `Scene` (`Scene::drives`).** Rejected: deleting a scene then means walking
every other scene in the project to prune references to it, and the linking panel becomes a join
rather than a list.

**`Canvas::scenes` as owned vectors.** Rejected: it contradicts the on-disk layout in ADR-0022,
which is one file per scene with a manifest, and makes the thing a scene file is a child of
ambiguous.

## Consequences

**Easier.** A vertical show is authored as a show. Each canvas's scene list is its own, which is
what the scenes panel already looks like it is showing. The linking panel is a table view over
`scene_links` with add and remove, and it is the only place links exist.

**Harder, and new.** The shell currently takes `canvases.front()` and says so in a comment — every
panel is implicitly working against canvas zero. Scenes, the sources panel and the properties
panel all become canvas-scoped, so "the canvas I am working on" has to stop being a placeholder
and become a real selection in the UI. That is the bulk of the work, not the model change.

**The command set grows and changes.** `AddScene` carries a canvas. `AddSceneLink` and
`RemoveSceneLink` are new. `SetNodeCanvasOverride` is deleted. The forty-command count moves, and
the README says forty.

**Must be tested.** A link whose target scene was deleted; a link whose target *canvas* was
deleted; the one-hop rule, with a three-canvas project arranged so a chain would close a cycle;
that designating an emergency scene refuses a scene that is the `to` of a link, and that a link
refuses an emergency scene; that a version 1 project with overrides loads, reports, and round-trips
as version 2; and the existing property test — random command sequences, undo and redo returning
the project byte for byte — extended over the two new commands.
