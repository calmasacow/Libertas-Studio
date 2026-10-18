<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Todo

Design decisions the operator has asked for that are **not built yet**, kept here so they are not
lost between sessions. Anything here that changes the document model needs an ADR before it is
written, because the model is what every other decision hangs off.

## A canvas you can see, per canvas, built on 2026-10-15

Every canvas now has a Program panel of its own — `program:<canvasId>`, Main keeping the bare
`program` — created and removed with the canvas, tabbed beside Main's. The frame takes its
shape from the canvas's own size rather than a fixed 16:9, so a vertical canvas draws portrait
and says its resolution. ADR-0027 has the reasoning.

### Still open here

- **A colour reaches it.** `libertas-render` composites the program scene and the panel shows
  that frame: a colour, a group of those, and a precomp made of those (ADR-0040). Anything else
  is left undrawn and the caption says how many. Capture, encode, audio and outputs are still
  absent, so this is a preview of the document, not a broadcast. The thirds and the framing
  guide are still there.
- **The three panels dock as a group of one each.** A canvas's three panels each go beside
  Main's matching one. There is no way to say "give me all of Vertical's panels in one place",
  which is what someone building a second show might actually want.

## No drag-scroll, 2026-10-17

Every `Flickable` in the shell is `interactive: false`, with the wheel given back by
`NoDragScroll.qml` ([ADR-0039](adr/0039-nothing-scrolls-by-being-dragged.md)). A list is a
list of things to grab, and a surface that slides when pulled will eventually slide while
someone is pulling on what is in it.

### Still open here

- **No scrollbars anywhere.** Nothing says a list has more below it, which matters more now
  that it cannot be flicked to find out. A thin indicator that appears while scrolling would
  do; a Controls `ScrollView` would not (ADR-0015).

## Selecting more than one source, built on 2026-10-17

Ctrl and Shift click in the Sources panel, `Ctrl+G` to group the selection
([ADR-0037](adr/0037-more-than-one-selected-source.md)). `selectedNode` still means the node
last clicked, so every panel that read it keeps working; `selectedNodes` is the whole list, in
scene order.

### Still open here

- **No rubber-band selection.** `Ctrl+A` exists; dragging a box over rows does not.
- **No auto-scroll while dragging** past the top or bottom of a long list
  ([ADR-0038](adr/0038-the-sources-panel-drags.md)).
- **Groups do not collapse**, so there is no spring-loading either — in Photoshop, hovering a
  closed group during a drag opens it.
- **Properties blanks above one selection** by the operator's choice. Editing all of them is
  the bigger feature: every field needs a mixed-value state and every setter becomes a batch.

## Copy and paste, built on 2026-10-17

`Ctrl+C` / `Ctrl+V`, and right-click menus on source rows and scene rows. A node pastes into
the current scene, a scene onto the current canvas keeping its name. Coordinates are preserved
rather than scaled, with a count-only warning when something would land off the canvas
([ADR-0036](adr/0036-the-clipboard-is-the-shells.md)).

### Still open here

- **No multi-select**, so copy takes one node at a time. The Sources panel has single
  selection; this would follow from that rather than lead it.
- **`pasteWouldMissCanvas` over-warns.** A node with no explicit box size counts as a point, so
  a source the renderer would scale to fill the frame may be reported as off-canvas. Over-
  warning is the right way round, and it gets honest when the renderer can answer the question.
- **A pasted scene's precomps still point at the same scenes**, which is usually right and can
  produce a cross-canvas precomp. Allowed by the model; worth watching once things render.

## A scene inside a scene, built on 2026-10-17

The add dialog's kind list gained "Scene": picking one adds a `Precomp` node, a whole scene
shown live inside another, on any canvas
([ADR-0034](adr/0034-a-scene-inside-a-scene.md)). The model already had the kind and the cycle
check; only the gesture was missing.

### Still open here

- **A precomp draws when its contents are things the preview can draw.** A precomp of colours
  shows those colours. A precomp of anything else leaves a gap and the caption counts it.
  Capture is still what would put a camera in it.
- **Deleting a scene that another scene shows says nothing.** `RemoveScene` predates this
  gesture. It should probably warn, the way removing a canvas with scenes refuses.
- **No way to change which scene a precomp shows** after it is added, and no size control on
  the node. Both are Properties-panel work.

## The source manager, built on 2026-10-17

The Add Source dialog is the manager ([ADR-0033](adr/0033-deleting-a-source-takes-its-placements.md)).
Right-click a source tile to delete it; one confirmation when it is unused, two when it is in
use, the second naming the scenes. Deleting removes the source **and** its placements as one
`Batch`, which is deliberately not what the bare `RemoveSource` command does.

### Still open here

- **The file is still called `AddSourceDialog.qml`.** Worth renaming when something else
  touches it; not worth a commit that only moves a file.
- **No rename from the manager.** `renameSource` exists and nothing in the manager calls it;
  renaming is still done from the Sources panel row.
- **Nothing stops a source being deleted while it is on air**, because nothing is on air. When
  the render path exists that is a question for the session process, not this dialog.

## The mixer shows sources, built on 2026-10-16

One row, two groups: source strips justified left, bus strips right, a divider immediately left
of the leftmost bus strip. A source strip appears because its node is in the scene on screen
and its *type* has audio — `ldoc::has_audio`, a table beside `is_self_contained`, taken from
OBS type by type. [ADR-0030](adr/0030-the-mixer-shows-sources.md) has the reasoning, including
why an unknown type answers "has audio" when `is_self_contained` answers "no".

### Still open here

- **No meters.** Nothing makes a sound yet, so a moving meter would be an animation on a timer.
- **No solo anywhere, deliberately** — removed from the document on 2026-10-16
  ([ADR-0031](adr/0031-no-solo-and-a-monitor-toggle.md)). Every strip carries mute and monitor
  and nothing else. If solo is ever asked for again, the reason it went is that it was document
  state: it would silence other buses *on air*, not just in the operator's headphones.
- **Nothing monitors anything yet.** `monitor` is document state the audio host will read, the
  same as the faders are. The rule it has to keep: monitoring never changes the program mix.
- **The closed eye reads as an umbrella.** `EyeIcon.qml` draws the lid as the lower half of
  the almond with lashes below, and at 14 pixels the flat top does not say "shut". Accepted
  during development on 2026-10-16 -- "it will get change for permanent icons later" -- and
  parked with the rest of the styling below.
- **The strips are not styled yet** — "that is fine we will make it pretty later", 2026-10-16.
  Correct in behaviour, plain on screen. The operator's reference (an Elgato Wave Link strip)
  carries a level meter beside the fader, a numbered dB scale down its side, and a headphone
  glyph on the monitor button. The meter waits on the audio host; the scale and the glyph do
  not. Note on the glyph: U+1F3A7 resolves to Noto Color Emoji here and colour emoji cannot be
  tinted, so a latched monitor would look identical to an unlatched one -- it has to be drawn,
  as `ChainIcon.qml` is, which is why the button says "H" today.
- **No way to change a strip's bus from the strip.** `routeNode` exists and is covered, but the
  only route control on screen is still the selected-node line along the bottom.
- **One mixer, not one per canvas.** Scenes and Sources are per canvas now; buses are
  project-level, so a per-canvas mixer would repeat the same destinations. Revisit if a canvas
  ever gets buses of its own.

## Per-canvas scenes and sources, with scene linking

**Answered by the operator on 2026-10-13, written up as
[ADR-0027](adr/0027-scenes-belong-to-a-canvas.md), and built on 2026-10-14 and 2026-10-15.**
All three numbered items below are done; the section is kept for the reasoning, and what is
still open is listed under "The canvas UI" and "A canvas you can see" above.

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

**Designed by the operator on 2026-10-13, with mockups. The Canvas panel, the Canvas Manager
and the Outputs Manager were built on 2026-10-14.** What remains unbuilt here is listed under
"What has to be answered before this is built" below: Backtrack as an output kind, hotkeys,
custom stream delay, and the full encoder sheet. Clone Canvas is deferred post-1.0.

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
  filed next to the 40 ms submission stall on the theory that it was the cure. The stall now has
  a negative at a sample size that means something (below), so **the case for building explicit
  sync has to be made from somewhere else** — the frame pacing it would buy, say — and not from
  a fault that no longer shows up.
- **A note ADR-0013 should carry, found on argus.** Under Hyprland, once a pending
  `ext-image-copy-capture` frame is destroyed, the *next* request is answered immediately with
  unchanged content. So a capture worker that implements a timeout as "cancel and ask again"
  invents frames: measured at a 1 s timeout on a screen genuinely redrawing every ~5 s, it
  reported a capture every 1002 ms, each one the timeout plus 2 ms, and filled the capture
  timings with the timeout's own length. `lcapture` must not do this. `encode-spike` watches for
  dry spells from the encoder side for exactly this reason, and `wlcap.hpp` says so at the
  function — but it belongs in ADR-0013, where someone writing the real worker would look.
- **`wlcap_capture_frame` still waits without a bound.** The single-shot variant has no callers,
  so the idle timeout was not wired into it. Wire it, or delete it, before something starts using
  it.
- **The portal path would mislabel a still screen.** On the PipeWire/portal backend, zero frames
  reports "timed out waiting for format negotiation" from the 8 s watchdog in `stream.cpp`, and
  capture-spike's fallback ladder could then drop a modifier to try to fix something that was
  never broken. It does not hang, and the case has not been seen (a portal screencast normally
  sends a first frame), so this is a wrong *message* rather than a wrong outcome. It needs a
  machine where someone can work the portal picker.

When a destination is finally needed for a real push, point it at a **local MediaMTX container**.
Never at a live key.

## Deferred by choice

- **Tweaks to the source settings panel.** The operator looked at the built panel on
  2026-10-13, said "there is some things that need tweaking", and on 2026-10-14 put it on
  the back burner as not yet finalised. Nothing is known about what the tweaks are, so there
  is nothing to guess at here — this entry exists only so the panel is not mistaken for
  finished.
- Asset commands.
- Compound editing beyond `Batch` (the 44th command).
- **The rest of the render path.** `lcomp` draws the program preview and `libertas-render`
  publishes it (ADR-0040). `lcapture`, `lmedia` and `laudio` are still empty: nothing captures,
  nothing encodes, nothing mixes audio. The transport underneath (`lipc`, ADR-0026) is the
  same one the preview already uses.

## The combined format bump, done on 2026-10-14

`kSchemaVersion` is **2**, and all three ADRs that were waiting for it landed together, in that
order: [ADR-0027](adr/0027-scenes-belong-to-a-canvas.md) (scenes belong to a canvas, links are
cues), [ADR-0029](adr/0029-an-encode-is-a-thing.md) (an encode is a document object an output
names) and [ADR-0028](adr/0028-one-word-for-a-source.md)'s wire half. Each ADR now carries an
implementation note recording what was decided while building it.

Everything the old "still owes" list named is done: `"kind": "layer"` is `"source"` on the wire
with `"layer"` still accepted on the way in; `layer.set-source` is `node.set-source` with the
old name still read, because the journal is replayed during crash recovery and that is the worst
possible place for a strict parser; and `kind_str()` in `document.cpp` and `kind_word()` in
`inspect.cpp` are deleted, with both call sites back on `to_string`.

The four `layer.*` action ids in `lsession` are **still deliberately untouched**
(`layer.delete`, `layer.move-forward`, `layer.move-back`, `layer.toggle-visible`). They are keys
in the operator's bindings TOML, which no `kSchemaVersion` covers, so renaming them would
silently unbind keys the operator chose. Their display names already say "source". Renaming them
needs either a migration of that file or an alias in `find_action`.

The golden fixture is now a pair. `golden-project-v1` is **frozen and must never be
regenerated** -- its value is that it was written by a build predating the change, so opening it
exercises the real migration rather than a reconstruction of what version 1 is believed to have
looked like. `golden-project-v2` is the byte-for-byte fixture, and carries a scene link, a
vertical scene of its own, and two encodes feeding three destinations.

### What this did not build

- **Start All / Stop All.** Nothing to start: no output actually runs yet, and the shell still
  reports "Start/Stop Streaming is not wired up".
- **Backtrack**, which has no document shape yet.
## The canvas UI, built on 2026-10-14

ADR-0027's UI half and ADR-0029's amendment, after the operator settled the four questions they
were waiting on. Each is recorded as an amendment on its ADR.

- **A Canvas panel**, a tab beside Outputs with the same `+ − gear` footer. The gear opens a
  Canvas Manager for name, size and rate.
- **A Scenes and a Sources panel per canvas** -- "Scenes: Main", "Sources: Vertical" -- created
  when the canvas is, removed when it goes, each with its own selected scene. Main keeps the
  plain `scenes`/`sources` panel ids, so an older layout file still loads.
- **Linking**, from a chain icon on the rows of a secondary canvas only. It asks which Main
  scenes bring this scene up, says before committing which links a tick would move, and
  commits the whole tick-list as one undo step.
- **An outputs manager** behind the Outputs gear: encodes with their destinations hanging off
  them, and `+ destination` to put another one on the same bitstream. Until this existed
  nothing in the shell could make two destinations share an encode at all.

### Still open here

- **Reordering canvases.** `AddCanvas`/`RemoveCanvas` exist and Main is pinned at index 0, but
  there is no move: a canvas list of any length is in creation order and stays there.
- **Moving a scene between canvases.** `Scene::canvas` is settled at creation. A canvas holding
  scenes cannot be removed and the refusal says "move or delete them first" -- which is
  currently advice with no gesture behind it.
- ~~**An encode of a canvas other than the current one.**~~ Built on 2026-10-17: each encode
  block in the Outputs Manager has a canvas dropdown
  ([ADR-0035](adr/0035-an-encode-names-its-canvas.md)).
- **Choosing an existing encode from an output row.** Deliberately not built: the operator chose
  the manager window over a per-row dropdown (ADR-0029 amendment). `setOutputEncode` exists and
  is covered, but nothing in the UI calls it -- the manager hangs new destinations off an
  encode instead of moving existing ones between encodes.

## Found by launching it, 2026-10-14

Running the shell against the v2 golden fixture, which is the first time the encode model was
looked at rather than tested.

- **The outputs row said the wrong resolution.** It printed the encode's *canvas*, so a 1080p
  stream off a 1440p canvas read as 1440p -- and downscaling is the ordinary case. It now says
  `Main -> 1920x1080`, the canvas and what actually leaves the encoder. Fixed.
- **The outputs row did not say an encode was shared.** Twitch and Kick showed identical
  numbers, which two *separate* encodes configured alike would also show. The row now names
  the encode and the other destinations on it. Fixed.
- **Removing an output left its encode behind.** `addOutput` creates the pair in one batch and
  said in a comment that nobody wants the encode left behind on undo -- but `removeOutput` was
  a bare `RemoveOutput`, so a few add/remove cycles silently accumulated encodes that nothing
  sends and that the inspector still counts against the GPU. Reproduced by the operator in
  about a minute of clicking. Removal now takes the encode with it when it was the last output
  on it, as one undo step. Fixed, and it closes the garbage-collection question that was open
  here: the answer is "when nothing else is sending it".
- **The self-check read the model, not the screen.** A role added to a model but not declared
  on its `required property` delegate renders the literal word "undefined" under
  `pragma ComponentBehavior: Bound` -- it does not fail to build and it does not fail any
  model-level assertion. That is exactly what the first cut of the resolution fix did, and the
  suite stayed green while the panel was wrong. The self-check now walks the live scene graph
  and fails if anything the operator can see says "undefined". Verified by breaking the
  delegate on purpose: it reports the offending string.
- **Model role indices were positional.** `out_at(row, 9)` and friends read by offset from
  `Qt::UserRole`, so inserting a role re-pointed every later check at the wrong column. They
  look roles up by name now; a green suite reading the wrong fields is worse than a red one.

## Verified on argus and the dev box, 2026-10-13 and 2026-10-14

Reached as `ssh -o HostKeyAlias=argus calmasacow@192.168.1.245` (`argus.lan` no longer resolves,
`argus.local` does). It builds with `-G "Unix Makefiles"`; there is **no ninja**, so the presets
in `CMakePresets.json` do not work there — configure by hand into `build-make`.

The source now lives in **`~/Development/Libertas-Studio`**, a full clone with history, on
branch `foundation`. The older `~/libertas-src` is a historyless rsync and is stale; ignore it.
argus has **no git identity configured**, so commits there need
`git -c user.name=... -c user.email=...`; nothing changed its config. Over SSH the environment
has no `WAYLAND_DISPLAY` and `XDG_SESSION_TYPE=tty`, so anything touching the display needs
`XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 XDG_SESSION_TYPE=wayland` — and
`libertas probe` correctly exits 2 without them (ADR-0003), which is not a regression.

- **Caught up from the `lsession` commit to the current tree**, uncommitted work included.
  Configure, build and all twenty tests green, including the shell self-check headless. Zero
  warnings, so `-Werror` holds there too. `QuickDialogs2` — the new dependency behind the Browse
  button — is present.
- **`libertas probe` is clean** (exit 0, one warning: QSV unavailable on `renderD129`).
  ADR-0008 still holds exactly: Vulkan video encode present on the RTX 2070, QSV failing on
  `renderD129` with `MFX session: -9` under both the default libva and `iHD`.
  The encoder table's "present" column, which said `av1_nvenc` and the three `*_qsv` entries were
  present when none of them work there, **is fixed** — see "Done" below.
- **`capture-spike` passes.** 300 frames in 5003.2 ms, 60.0 fps, real pixel content imported into
  Vulkan on an explicit modifier. Per frame mean 16.68 ms, p99 19.71, max 22.17.
- **The encode gate passes, and the wall-clock accounting is verified.** 600 frames in 9.99 s
  (60.04 fps), `h264_nvenc` at 2560x1440. Audio held 9.984 s against 9.993 s of wall time, a drift
  of −8.7 ms over ten seconds. The accounting closes: capture loop 16.678 ms wall against 16.677
  named, 0% unaccounted; encode loop 1.493 against 1.429, 4%.
- **The 40 ms submission stall did not reproduce, at a sample size where that means something.**
  [ADR-0019](adr/0019-obs-dual-gpu-goes-through-system-ram.md) put it at roughly **2 events in
  8000 captures**, located in `record` — host-side command-buffer building inside
  `vk_convert_nv12`, with the GPU itself taking 0.003 ms — and named a driver or power-state
  transition after GPU idleness as the leading suspect. So: ten runs of 6000 frames through
  `encode-spike`, which is the tool that times the convert, with audio on (ADR-0019 had already
  tested and rejected the audio hypothesis, and both original events had it) and the GPU left
  deliberately idle for 90 s between runs. **59,940 timed converts**, against a rate that
  predicts about fifteen events.

  | | worst in the series | ADR-0019's event |
  |---|---|---|
  | convert | 5.721 ms | 40.475 ms |
  | its `record` term | **0.463 ms** | **39.375 ms** |

  Two converts over 5 ms in 59,940. **Zero over the 16.667 ms budget, in every one of the ten
  runs.** Mean convert 2.18 ms and p99 2.9 ms, identical run to run to within 0.01 ms, including
  the first run after each idle period. Whatever ADR-0019 caught in October is not happening on
  argus now.

  **A correction to what this file said yesterday.** The run queued overnight used
  `capture-spike`, which never converts — so whatever it printed, it could not have seen this
  fault, and the script's own header said "ADR-0019's event is a ~40 ms convert" while running
  the wrong tool. It was worth having anyway: it is the 100,000-capture confirmation now folded
  into [ADR-0013](adr/0013-capture-is-damage-driven.md), and it is the reason the 40–50 ms
  *inter-arrival* maxima it reported are known to be the content rather than the capture path.
  But it was the wrong instrument for this question and was written up here as though it were
  the right one.

- **The per-stage shape of a healthy run**, worth having written down because it is what a sick
  one would be compared against. argus: worst convert 4.008 ms, submit 1.208, 0 of 599 over
  budget. The dev box (RTX 5080, DP-1, same run with the operator's consent): worst convert
  2.015 ms = record 0.044 + submit **0.019** + gpu-wait 0.291 + copy 1.660, 0 of 380 over budget,
  wall-clock accounting 0% and 6% unaccounted, A/V drift −4.3 ms over ten seconds.

**A real trap, and the tool bug behind it — both now fixed.** Capture is damage-driven
(ADR-0013), and nobody is sitting at argus, so the first run of `capture-spike` waited for ever
on a screen that never changed, printing nothing and explaining nothing. Both spikes need
something moving on the display; a `foot` window running a print loop is enough. They now say so
instead of hanging — see "Done" below. Also worth knowing before planning a long run there:
**argus's "idle" desktop is not still.** Something redraws HDMI-A-1 about every 5 s, confirmed
against the compositor's own presentation timestamps, so a few thousand frames at that rate is
hours rather than minutes.

## Done since this file was written

### Landed overnight on argus, reviewed and merged 2026-10-14

Three headless tasks ran unattended on argus while the dev box was off, each on its own branch
from `foundation`. All three were reviewed here, rebuilt under the `linux-ci` preset (`-Werror`)
and merged; twenty tests green.

- **[ADR-0028](adr/0028-one-word-for-a-source.md), the C++ half.** `NodeKind::Layer` →
  `Source`, `LayerData` → `SourceData`, `SetLayerSource` → `SetNodeSource`, and across the UI
  bridge `Document.layers` → `nodes`, `selectedLayer` → `selectedNode`, `addLayer` →
  `addSourceNode`, the whole `setLayer*` family → `setNode*`. The bridge says **node**, not
  source, because those calls act on whatever is selected and that includes groups. The
  layer/source explanation moved out of `SourcesPanel.qml` into `node.hpp` at `SourceData`.
  The serialised words did not change — see "What the combined format bump still owes".

  **The ADR was wrong about one thing, and it has been corrected in place:** it said nothing the
  operator sees would change. In fact "layer" was on screen in the undo history, the Properties
  panel, the mixer, the Info panel, the settings dialog, an emergency-scene warning and four
  shortcut names. All now say "source". **One wording worth a second opinion:** `RemoveNode`'s
  undo text is "Remove from scene" and the shortcut is "Remove From Scene", because
  "Remove source" is already what `RemoveSource` means — deleting it from the project.

- **The spikes no longer hang in silence on a still screen.** `--idle-timeout SECS` (default 10,
  `0` waits for ever) on both spikes. `capture-spike` exits **7** with an error naming the cause,
  so a script can tell "nothing moved" from "capture is broken"; `encode-spike` warns once when
  captures dry up and once when they come back, and keeps encoding, because duplicating the last
  frame is correct (ADR-0013). The timeout is deliberately *not* used to re-request a frame —
  see the ADR-0013 note under "Needs the operator".

  Two parsing faults were found here on review and fixed: `--idle-timeout nope` read as 0 and so
  meant "wait for ever", restoring the exact hang the flag exists to end (`strtod` returns 0 for
  text it cannot read, and the end pointer was being discarded); and `0.0004` truncated to 0 ms
  and meant the same thing. Any positive value now means a timeout.

- **`libertas probe` no longer prints "present" as though it meant "works".** The encoder table
  now has two columns — **built**, a fact about this libavcodec, and **this machine**, which
  cites what the Vulkan, VAAPI and QSV sections actually found: `ran`, `FAILED`, `no`, `claimed`,
  `unknown` or `software`, each defined in a legend above the table. The table runs *after* those
  sections so it can cite them, the Vulkan section now reports the per-codec encode extensions
  rather than only `VK_KHR_video_encode_queue`, and `h264/hevc/av1_vulkan` were added because the
  primary encode path (ADR-0006) was not in the list at all.

  Checked on both machines, and the two now read differently, which is the point:

  | | argus (RTX 2070) | dev box (RTX 5080) |
  |---|---|---|
  | `av1_nvenc` | **no** — lacks `VK_KHR_video_encode_av1` | **claimed** |
  | `h264_qsv` | **FAILED** — no QSV session on `renderD129` | **ran** — encoded 1 s, with `LIBVA_DRIVER_NAME=iHD` |
  | `h264_vaapi` | **unknown** — no `vainfo` installed | **claimed** |

  `h264_qsv` is the only row that can ever read **ran**, because the QSV section really does
  encode a second of H.264. Everything else on NVIDIA is the driver's word, and says so.

  **Ready follow-ups, deliberately not done:** HEVC and AV1 VAAPI entrypoint checks (one `vainfo`
  run per node, read for `VAProfileHEVCMain` / `VAProfileAV1Profile0` beside
  `VAEntrypointEncSlice`) — skipped because argus has no `vainfo`, but **the dev box has one**,
  so this can be written and tested here. And an opt-in `probe --encode-test` that turns
  "claimed" into "ran" by opening each encoder in a child process with a hard timeout and
  sending it a frame — some hardware encoders open cleanly and only fail on the first frame.

### Also done here

- **The double-click is now actually double-clicked.** The self-check opened the settings panel
  by calling the same function the handler calls, which tested the panel and not the gesture —
  and the rename went straight through that handler. It now posts a real press / release /
  press / dblclick / release through the window, on a bound row (settings open, no rename) and
  on a group (no panel, rename starts), because those are the handler's two branches and they
  mean opposite things.

- **The last two places that still said "input".** The Inputs panel was folded into Sources a
  while ago, but `libertas inspect` still printed a heading called "inputs" and
  "(missing input)", and `document.hpp` had a section header to match.


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
