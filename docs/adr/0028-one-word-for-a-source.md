<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0028 — One word for a source, in the code as well as on the screen

- **Status:** accepted
- **Date:** 2026-10-13
- **Depends on:** ADR-0027 (scenes belong to a canvas) — they share one format bump

## Context

The screen settled on one word some time ago: anything that can go into a scene is a **source** —
a capture card, a webcam, a browser, a flat colour — differing by type, not by kind. The separate
"Inputs" panel was a second word for the same thing and was folded into Sources.

The code did not follow. A source placed in a scene is a `Node` whose kind is `NodeKind::Layer`
and whose payload is `LayerData`. The split was deliberate and is defensible: placing a source
gives it a transform, layout, blend and effects that the source itself does not have. Same thing,
two amounts of information.

It is also the only place in this program where the screen and the code use different words for
what the operator thinks of as one thing, and the entire explanation of the difference lives in a
comment at the top of `SourcesPanel.qml`. Asked, the operator's answer was to drop it: one word is
less confusing.

**The complication, stated plainly, because it does not go away.** There really are two things.
One camera is opened once and can sit in six scenes at six positions with six sets of effects
(ADR-0027 §4 makes that explicit). So this is not a merge of two types into one. It is a decision
about **which of the two keeps the word**, and the other one needs a name that is not "layer".

## Decision

The scene tree keeps being a tree of nodes. A node is a group, an adjustment, a mask, a reference
— or a **source**:

- `NodeKind::Layer` → `NodeKind::Source`
- `LayerData` → `SourceData`
- `SetLayerSource` → `SetNodeSource` (the only command carrying the old word; every other one is
  already `SetNode*`)
- `Document::addLayer` → `Document::addSourceNode`, and the QML-facing `layers` model and
  `layerProperties` follow

The project-level `Source` keeps its name, its type ids (`source.v4l2`, `source.browser`,
`source.color`) and its place in `Project::sources`. The word "layer" leaves the codebase.

```
scene
└─ Node { kind: Source, data: SourceData }
     └─ source: Id  ──►  Project.sources[]
                           Source { name, type, settings }
```

Read aloud: **a source node bound to a source**. The node is where it is and what is being done to
it; the source is what it is.

`Source` and `SourceData` differ by four characters, and that is the cost of the decision. It is
paid in one place: the comment currently at the top of `SourcesPanel.qml` moves to `node.hpp`,
where the two types actually meet, and says which is which.

## Rejected alternatives

**Leave it as two words.** The position until today, and the one this file's author argued for:
the distinction is real, so the code may as well name it. Rejected by the operator, and the
counter-argument is better than the argument — the screen says "source", the code says "layer",
and the one document reconciling them is a comment in a QML file that nobody reading the C++ will
ever open.

**Give `Source` to the scene item and rename the project-level one to `SourceDef`.** Closer to one
word in the place the operator actually looks. Rejected because the project-level type is the half
that is *finished*: the add dialog, the recently-created list, the use counts, the settings schema
and every `source.*` type id are built around it being `Source`. This option moves all the churn
onto the working half to spare the half that is about to be rewritten anyway.

**Collapse the two types into one.** Rejected on the operator's own answer: one camera used twice
is one camera, not two. Two placements of one device is the requirement, so two types is the
model. A single type would have to carry either one transform shared by six scenes or a map of
transforms keyed by scene, which is the second type wearing a disguise.

## Consequences

**One format bump, not two.** This change and ADR-0027 both alter what is on disk, so they ship as
one `kSchemaVersion` 2 with one migration and one set of fixtures. Doing them separately would
mean two migrations, and the second would have to be written against the first.

**Wide but mechanical.** `ldoc` (node, command, json, validate), `lsession`, the `libertas-ui`
bridge and models, every panel that says "layer", the CLI `inspect`, and the tests. The compiler
finds all of it; the JSON reader is the one place it will not, since field names are strings.

**Must be tested.** That a version 1 project whose nodes say `"kind": "layer"` loads as source
nodes and round-trips as version 2; that `serialise → parse → serialise` remains a fixed point
across the rename; and that the existing property test — random command sequences asserting undo
and redo return the project byte for byte — passes unchanged over the renamed command.

**What does not change.** Nothing the operator sees. That is the point of doing it: the screen was
already right.
