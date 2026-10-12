<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0021 — Layout is workspace state, with its own library and its own undo stack

- **Status:** accepted, implemented
- **Date:** 2026-10-12
- **Implements:** ADR-0015 (the UI owns its layout), ADR-0016 (Adobe drop zones)
- **Amends:** ADR-0015 rule 6, and the repo tree in the plan

## Why this exists

ADR-0015 and ADR-0016 specified the behaviour. Building it forced six decisions they did not
cover, and one of them is a correction to ADR-0015 itself. They are recorded here rather than in
commit messages because each one will look arbitrary to whoever reads the code next.

## 1. `libs/llayout/` is its own library, and the plan's tree is wrong about this

The plan puts the document model in `libs/ldoc/` and does not mention a layout library, which
implies layout belongs in the document. It does not.

A document is the show: scenes, sources, buses, canvases, outputs. It is what gets shared, copied
between machines, imported from an OBS collection and version-controlled by the careful. A layout
is how *this* operator has arranged *this* installation's panels. Putting them in one file means a
project opened on the streaming rig rearranges the editing machine's workspace, and a project
shared with someone else carries a layout built for a triple-head desk.

So layout is **workspace state**: separate library, separate file, no dependency on `ldoc`. It
also means the layout engine is testable without a document model existing, which is why it exists
first.

## 2. Layout gets its own undo stack — ADR-0015 rule 6 was too broad

ADR-0015 rule 6 says "layout is a command like any other, so rearranging panels is undoable". The
first half of that is now wrong and the second half is right.

Adobe — the model the operator asked for — does not put workspace changes in the history panel.
The reason is worth stating: if rearranging a panel and deleting a scene share one undo stack,
Ctrl+Z stops being a keystroke the operator can press without looking first. Mid-show that is the
difference between a safe reflex and a gamble.

`llayout::History` is therefore a separate, bounded, snapshot-based stack. Snapshots rather than
inverse commands because a layout tree is a few dozen nodes — cheaper than the bookkeeping, and
incapable of drifting out of step with the thing it describes, which is what makes undo stacks
untrustworthy in the first place.

Two consequences that matter in use:

- A gesture that changed nothing does not spend a step. Otherwise Ctrl+Z after a mis-aimed drag
  appears to do nothing, the operator presses it again, and the second press undoes real work.
- A divider drag is **one** step, not one per mouse move.

## 3. Nearest-edge hit testing, which is provably ADR-0016's partition

ADR-0016 writes the four outer zones as inequalities on normalised `u, v`. The implementation
instead picks the nearest edge. These are the same partition: if `v < u` and `v < 1-u` then
`v < min(u, 1-u) ≤ 0.5 = 1-v`, so the nearest-edge test adds nothing the diagonals had not already
imposed.

Nearest-edge is used because it resolves the **corners**, where two zones meet at a single point.
The four inequalities name no zone there at all, so an implementation of them falls through to
whichever branch happens to be last — which is how `(0, 1)` ends up reported as `Right`. A test
sweeps the panel and asserts the two formulations agree on every point where the ADR's strict
inequalities name a zone.

Ties break horizontally. At an exact corner either answer is defensible; side docking is the more
common gesture in a video application, which wants tall columns.

## 4. Docking along the parent's own axis inserts a sibling instead of nesting

A drop on the left of a panel whose parent is already a horizontal split takes half of that
panel's slot and becomes its sibling. It does not create a nested horizontal split inside the
slot.

Without this, every dock adds a level. The tree grows without bound under ordinary use, and —
visibly — the dividers stop lining up with each other, because a nested split's divider is
positioned within its parent's slot rather than across the row. Twelve docks along one axis now
leave the tree four levels deep.

The same rule, applied at the root, is what makes an edge dock produce a full-height column rather
than a column nested inside a row.

## 5. Every mutation runs on a copy and commits only if it validates

ADR-0016 rule 8 asks for refused drops. The harder requirement is that a refused drop changes
*nothing*, and the hardest bug this code can have is a half-applied tree mutation — a dangling
child, a fraction vector of the wrong length, a panel in two groups at once.

So `dock()` clones the tree, detaches, normalises, inserts, normalises again, runs `validate()`,
checks minimum-size feasibility, and only then commits. Cloning a twenty-node tree costs nothing
and the property test gets to assert, after every one of several thousand random gestures, that a
rejection left the layout byte-identical.

## 6. `resolve()` adapts the geometry; it never rewrites the fractions

When a window gets too small for a split's children, the obvious move is to rewrite the fractions.
That is the wrong one: the operator set those proportions and would find them silently changed on
the next launch, which is indistinguishable from the "randomly rearranging things" this project
exists to stop.

Instead the fractions stay the operator's **intent** and `resolve()` water-fills against the
minimum sizes — hand out by fraction, pin anyone below their minimum, redistribute the rest. Make
the window bigger and the original proportions come back exactly, because they were never lost.

## 7. QML singletons, not context properties

The shell initially injected `workspace` and `theme` as context properties. `qmllint` then flagged
every single access to them as unqualified, and it was right to: a context property is invisible
to the linter and to the QML compiler, so a mistyped property name is a runtime warning in a log
nobody reads rather than a build failure.

Declaring both as `QML_ELEMENT QML_SINGLETON` types makes the whole shell statically checked and
compilable. `qmllint` now reports nothing at all, which means it will be worth running again.

## What this bought, measured

- `llayout` is 4 source files and has no dependency on Qt, a compositor, or a window.
- 73 cases across 5 binaries, including two property tests that drive several thousand random
  gestures and assert every invariant after each one.
- One engine bug found that only a property test finds: dropping a panel onto its own tab group's
  centre reported `NoOp` while having brought that tab to the front. Structure unchanged is not
  the same as nothing changed, and a UI trusting that status would have skipped both the repaint
  and the undo entry.
- The shell has a `--check` mode that loads the real QML tree, drives the real gestures through
  the real engine and writes a real layout file, without showing a window. It runs in CI, and on
  a live session it stays off the operator's screen.

## Still open

- **Tear-out to a floating window**, deferred by the operator until the docked model is complete
  (ADR-0016). The engine already supports a detached tree per toplevel; what is missing is the
  gesture and the `xdg_toplevel_tag_manager_v1` tagging.
- **Panel contents.** Everything inside a panel is a placeholder that says so. Drawing convincing
  fake meters would make it impossible to tell later which parts actually work.
- **A real list model.** Panels and dividers reach QML as a count plus an indexed accessor, with a
  revision counter driving the bindings. It keeps delegates alive across a divider drag, which is
  the requirement, but a `QAbstractListModel` would update only the rows that changed instead of
  re-evaluating all of them.
- **Keyboard-only docking.** The whole gesture is pointer-driven. An operator who cannot use a
  pointer precisely cannot currently rearrange the workspace at all, and "the layout engine has to
  be good" (ADR-0015) has to include that.
