<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Todo

Design decisions the operator has asked for that are **not built yet**, kept here so they are not
lost between sessions. Anything here that changes the document model needs an ADR before it is
written, because the model is what every other decision hangs off.

## Per-canvas scenes and sources, with scene linking

**Answered by the operator on 2026-10-13 and written up as
[ADR-0027](adr/0027-scenes-belong-to-a-canvas.md). Not built yet.**

The answer was simpler than this file guessed. Canvases are completely independent, each owning
its own scene list, and nothing about a scene's *contents* ever crosses between them. A link is
not a sharing relationship at all — it is a cue: activating a linked scene on the leading canvas
changes what the other canvas has on air, one way, one hop. A source used on both canvases stays
one source opened once.

What that leaves to build, in the order it has to happen:

1. The model change itself — `Scene::canvas`, `Project::scene_links`, the deletion of
   `Node::canvas_overrides`, two new commands and one deleted one, and the version 1 → 2
   migration. Shares its format bump with [ADR-0028](adr/0028-one-word-for-a-source.md).
2. **The canvas the shell is working on.** Today every panel implicitly works against
   `canvases.front()`, with a comment saying so. Once scenes belong to canvases that has to become
   a real selection the operator makes. This is the bulk of the work, not the model change.
3. The linking panel, which has not been designed. It is a table view over `scene_links` — add,
   remove, and show which way each one runs — but where it lives in the workspace is open.

## The canvas panel, and the two manager dialogs

**Designed by the operator on 2026-10-13, with mockups. Not built; most of it waits on
[ADR-0027](adr/0027-scenes-belong-to-a-canvas.md).**

### The panels

A **Canvas panel**, shaped like the Outputs panel, listing the canvases and defaulting to Main.
Both panels get the same three controls along the bottom: `+`, `−`, and a gear. On either panel,
`+` or the gear opens that panel's manager dialog — the same gesture the Sources panel's `+`
already uses to open the add-source dialog.

### Canvas Manager

A modal: "Add Canvas" top right, then one collapsible section per canvas, OK and Cancel at the
bottom. Each section carries **Name**, **Type** and **Resolution**; every canvas but Main also
gets a "Remove" button. Main's type reads **Default Canvas** and is fixed. Others are
**Extra Canvas**; the mockup's **Clone Canvas** option is deferred (see below).

### Output Manager

A modal: "Add Output" top right opening a menu of **Stream, Record, Backtrack, Virtual Camera,
FFmpeg**. Below that a row of "Show in output dock" toggles, then one row per output — icon,
expandable name, "Output Settings", "Remove", and reorder arrows.

Expanded, an output shows: the **Canvas** it belongs to, **Start/Stop hotkeys** with revert,
clear, add and remove, **Custom Stream Delay**, and **Custom Encoder Settings** — which opens
tabs for video and audio carrying **Reuse Output Video Encoder**, **Video Encoder**, and then the
full encoder sheet (colour format, space and range, FPS, rescale with downscale filter, rate
control, bitrate, keyframe interval, preset, tuning, multipass, profile, look-ahead, adaptive
quantisation, B-frames, custom options).

### What has to be answered before this is built

1. **Clone Canvas is deferred by the operator — back burner, possibly post-1.0.** The Type
   dropdown keeps **Extra Canvas**, which is the independent canvas ADR-0027 describes.

   Worth recording now so it is not rediscovered: in Aitum a clone is **not** simply a mirror.
   It clones another canvas *and substitutes sources* — its dialog is a two-column table of
   "Source to replace" against "Replacement Source" (`CanvasCloneReplace` /
   `CanvasCloneReplacement`, `src/docks/canvas-clone-dock.cpp`). So the same layout runs on a
   second canvas with, say, a different camera in the same hole. That is a more specific and more
   useful feature than "mirror Main", and it is a different question from scene linking rather
   than a special case of it.
2. **"Reuse Output Video Encoder" is one encode feeding many destinations**, not one encoder
   configuration shared across mismatched canvases. Settled, and written up as
   [ADR-0029](adr/0029-an-encode-is-a-thing.md): an encode becomes a first-class document object
   and an output names one. Rides the same format bump as ADR-0027 and ADR-0028 if built with
   them.
3. **Start All / Stop All, asked for by the operator on 2026-10-13.** Wanted regardless of how
   encoders are modelled: one button to go live everywhere, one to come down. Under
   [ADR-0029](adr/0029-an-encode-is-a-thing.md) they need no ordering — every output ensures its
   encode is running, and an encode stops when nothing running references it — which also removes
   the partial-failure state where one output fails to start because another was slow.

   Dependency ordering, and stopping a destination to stop its children, were asked for first
   and then **withdrawn by the operator once it was clear ADR-0029 removes the dependency
   itself**. Start All and Stop All are order-free. If "Kick only runs while Twitch runs" is ever
   wanted as a deliberate policy rather than as a side effect of a borrowed encoder, it gets its
   own feature — linked outputs, stated explicitly, available on any pair.

   Agreed either way: **label the relationship on the output row.** Under ADR-0029 the useful
   label is "shares encode `1080p60 8 Mbps` with Kick, Facebook" rather than "depends on
   Twitch" — the pre-flight question is how many encodes the GPU is being asked for.
   Consequence: name an encode for what it is (`1080p60 8 Mbps`), never for a destination, or
   stopping Twitch while "Twitch 1080p" keeps running reads as a bug.
4. **Backtrack** is a new output kind (OBS's replay buffer). It needs a document shape of its own:
   how much is held, and where it is written when it is triggered.
5. **Where does the Canvas panel live by default**, in Build, in Live, or in neither?

### The operator's own concern, recorded because it is right

> we are at the point where we will have to address the lack of menus. Eventually, we'll end up
> with too many docks stacked across the bottom the way they are. I'm not ready to remove that,
> but I am concerned about it.

Every new feature so far has arrived as another panel, and the row of "+ Favourites / + Outputs /
+ Broadcast Stats / + Shortcuts" buttons along the bottom is where that pressure is showing.
Canvas makes five. Nothing is being changed about it yet, deliberately — but a menu bar, or some
other home for things that are commands rather than surfaces, is now a real design question and
not a cosmetic one.

## Floating and popup panels

Output settings — destination, encoder, credential reference — should move out of the panel body
into a popup or floating panel rather than sitting in the interface. Deferred by the operator
until the docked model is finished.

The pattern it would use now exists: `SourceSettingsDialog.qml` is a modal drawn inside the one
surface, parented to the window's content item, holding its edits until OK and committing them
as a single command. An outputs version is that file with a different schema behind it.

Related and also deferred: tearing a panel out to float as its own toplevel (ADR-0015 rule 3).

## Needs the operator, or one of their machines

Not blocked on design — blocked on something only the operator can approve or power on.

- **Portal global shortcuts — deferred by the operator on 2026-10-13, and nothing is lost by
  waiting.** The bindings table already carries both scopes and the application-scope keys work
  today; only registering *system-wide* keys through the portal is unexercised, and those belong
  in the session process rather than the frontend anyway (ADR-0024). Picking this up later costs
  no rework.
- **Explicit sync via `linux_drm_syncobj_v1`.** Not a measurement that was waiting on a machine:
  nothing in this repository uses `syncobj` at all, so there is nothing to measure yet. It was
  filed next to the 40 ms submission stall on the theory that it was the cure; the stall does
  not reproduce on argus (below), so the case for building it has to be made again from
  somewhere else before it is.
- **The 40 ms submission stall still has no verdict, and a short run cannot give it one.**
  ADR-0019 recorded it as roughly **2 events in 8000 captures**, not reproducible on demand, with
  a driver or power-state transition after GPU idleness as the leading suspect. The runs on
  2026-10-13 saw about a thousand captures across both machines and found nothing — which is
  what that base rate predicts whether the fault is still there or not, so it is evidence of
  almost nothing. A real answer needs **8000+ captures per machine**, ideally with the GPU left
  idle first, which is minutes of continuous screen capture rather than ten seconds. Worth doing
  deliberately, not worth inferring from the gate runs.

When a destination is finally needed for a real push, point it at a **local MediaMTX container**.
Never at a live key.

## Deferred by choice

- Asset commands.
- Compound editing beyond `Batch` (the 40th command).
- **The render path as product code** — `lcapture`, `lcomp` and `lmedia` wired into
  `libertas-render`. The largest remaining piece of work; its transport foundation (`lipc`,
  ADR-0026) exists and is tested headless.

## Answered, written up, not yet built

- **One word for a source, in the code too.** The operator's call on 2026-10-13: drop the
  layer/source split from the code as well as the screen. Written up as
  [ADR-0028](adr/0028-one-word-for-a-source.md) — the scene-tree node becomes a source node
  (`NodeKind::Source`, `SourceData`) and the project-level `Source` keeps its name. Wide but
  mechanical, and it rides ADR-0027's format bump so there is one migration rather than two.

## Verified on argus and the dev box, 2026-10-13

Reached as `ssh -o HostKeyAlias=argus calmasacow@192.168.1.245` (`argus.lan` no longer resolves,
`argus.local` does). It builds with `-G "Unix Makefiles"`; there is no ninja. The source lives in
`~/libertas-src` as a plain rsync'd copy with no git, built into `build-make`.

- **Caught up from the `lsession` commit to the current tree**, uncommitted work included.
  Configure, build and all twenty tests green, including the shell self-check headless. Zero
  warnings, so `-Werror` holds there too. `QuickDialogs2` — the new dependency behind the Browse
  button — is present.
- **`libertas probe` is clean** (exit 0, one warning: `XDG_SESSION_TYPE` reads `tty` over SSH).
  ADR-0008 still holds exactly: Vulkan video encode present on the RTX 2070, QSV failing on
  `renderD129` with `MFX session: -9` under both the default libva and `iHD`.
  **Worth knowing:** the encoder table lists `av1_nvenc` and the three `*_qsv` entries as
  "present" when none of them work on this machine. It reports what libavcodec was compiled
  with, not what the hardware can do — which is the exact confusion ADR-0008 exists to correct.
- **`capture-spike` passes.** 300 frames in 5003.2 ms, 60.0 fps, real pixel content imported into
  Vulkan on an explicit modifier. Per frame mean 16.68 ms, p99 19.71, max 22.17.
- **The encode gate passes, and the wall-clock accounting is verified.** 600 frames in 9.99 s
  (60.04 fps), `h264_nvenc` at 2560x1440. Audio held 9.984 s against 9.993 s of wall time, a drift
  of −8.7 ms over ten seconds. The accounting closes: capture loop 16.678 ms wall against 16.677
  named, 0% unaccounted; encode loop 1.493 against 1.429, 4%.
- **Nothing resembling the 40 ms stall appeared**, on either machine — but see the open item
  above before reading anything into that. argus: worst convert 4.008 ms, submit 1.208, 0 of 599
  over budget. The dev box (RTX 5080, DP-1, same run with the operator's consent): worst convert
  2.015 ms = record 0.044 + submit **0.019** + gpu-wait 0.291 + copy 1.660, 0 of 380 over budget,
  wall-clock accounting 0% and 6% unaccounted, A/V drift −4.3 ms over ten seconds. The sample is
  too small to close the item; it is recorded because it is the per-stage shape of a healthy run
  on both machines, which is worth having written down.

**A real trap for next time, and a tool bug.** Capture is damage-driven (ADR-0013), and nobody is
sitting at argus — so the first run of `capture-spike` waited for ever on a screen that never
changed, printing nothing and explaining nothing. Both spikes need something moving on the
display; a `foot` window running a print loop is enough. The spikes should say so after a few
seconds of no frames rather than hanging silently, which they do not.

## Done since this file was written

- **The gear, and what is behind it.** Every source type now has a settings panel: a preview, a
  field per setting, and Defaults / OK / Cancel. Reached from the gear in the Sources panel
  footer or by double-clicking a row; renaming a row moved to F2, since double-click now means
  this. The fields come from one table in `document.cpp` rather than a hand-built panel per
  type, and a new source is created holding that table's defaults in full — the project format
  stores defaults rather than omitting them, and a source created with an empty settings map was
  the sparse case arriving by the back door.

- **One word for a source.** Anything that can go into a scene is a source — a capture card, a
  webcam, a browser, a flat colour — differing by *type*, not by kind. The separate "Inputs"
  panel was a second word for the same thing and has been folded into the Sources panel's add
  menu, where OBS puts it. The document model needed no change: `ldoc::Source` was always the
  source, and the type ids were always `source.v4l2`, `source.browser`, `source.color`.
