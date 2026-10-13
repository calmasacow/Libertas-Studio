<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Todo

Design decisions the operator has asked for that are **not built yet**, kept here so they are not
lost between sessions. Anything here that changes the document model needs an ADR before it is
written, because the model is what every other decision hangs off.

## Per-canvas scenes and sources, with scene linking

**Asked for:** each canvas has its own scene list and its own source list. Scenes can then be
*linked* between canvases, so that editing the "Chat" scene on the main canvas changes the linked
"Chat" scene on the vertical canvas.

**Why this needs an ADR rather than just code:** it is a different shape from what the document
model does today, and the difference is deliberate on both sides.

- **Today (ADR-0020, ADR-0022):** a scene is canvas-*agnostic* and project-level. One "Chat"
  scene exists once, and what makes it look right on a portrait canvas lives in each node's
  `canvas_overrides` rather than in a second copy of the scene. The reasoning was that two copies
  of a scene drift, and the one that drifts is always the one nobody is looking at.
- **Asked for:** two scenes, one per canvas, kept in step by an explicit link. This matches how
  the work actually feels — a vertical show is not the horizontal one squashed, it is a different
  arrangement of the same sources — and it is how the vertical workflow is described in practice.

They are not irreconcilable. The likely answer is that **a link is a declared relationship with a
stated scope** — which properties follow the link and which are local to the canvas — rather than
either full sharing or full duplication. The questions an ADR has to answer:

1. When a layer is added to the linked scene on one canvas, does it appear on the other? (Almost
   certainly yes, or the link is not worth having.)
2. When it is *moved* or resized, does it follow? (Almost certainly no — that is the whole reason
   the second canvas exists.)
3. What happens to a link when one side is deleted, renamed, or has a layer removed that the other
   still shows?
4. Does a source list per canvas mean two camera instances, or one instance referenced from two
   lists? One device opened twice is a real failure mode, so this probably stays a reference.
5. How does this interact with `Canvas::emergency_scene`, which is already per-canvas and already
   requires that a safety net cannot be shared (a per-canvas override can reposition a card but
   cannot swap what it is bound to).

**Confirmed and already true:** there is exactly one emergency scene per canvas.

## Floating and popup panels

Output settings — destination, encoder, credential reference — should move out of the panel body
into a popup or floating panel rather than sitting in the interface. Deferred by the operator
until the docked model is finished.

Related and also deferred: tearing a panel out to float as its own toplevel (ADR-0015 rule 3).

## Done since this file was written

- **One word for a source.** Anything that can go into a scene is a source — a capture card, a
  webcam, a browser, a flat colour — differing by *type*, not by kind. The separate "Inputs"
  panel was a second word for the same thing and has been folded into the Sources panel's add
  menu, where OBS puts it. The document model needed no change: `ldoc::Source` was always the
  source, and the type ids were always `source.v4l2`, `source.browser`, `source.color`.
