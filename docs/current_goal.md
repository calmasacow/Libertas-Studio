<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Where the work is, and what is next

Written 2026-10-18, at the end of a session that got a real screen drawing in the Program panel
and then went looking for why it is slow. Read this before picking the work up elsewhere.

## The state of the tree right now

`main` is at `b8e15c3`, pushed. **There are uncommitted changes in the working tree** — about
eighteen files. They build warning-free and the suite is 24/24. They were held back while the
composite was still slow for an unexplained reason; that reason is now found and fixed (see
below), so they are ready to commit once the `DIAG` instrumentation is stripped.

Two files still carry temporary `DIAG` instrumentation that must not be committed as-is:

- `libs/lcomp/src/composite.cpp` — timing around the walk, `paint_image`, `paint_screen`, the
  frame fetch and the blit, plus an alpha probe and a geometry probe.
- `libs/lcomp/src/preview.cpp` — timing around `composite_canvas`.

They print to stderr every sixtieth frame. Strip them before any commit, or keep them while
chasing the open problem below — they are how the numbers in this document were obtained.

## What landed and is pushed

Four commits put a live screen on the canvas, in this order:

- `2c31475` — ADR-0041, a screen source stores the connector it captures (`output`, e.g.
  `DP-1`). `libs/lcapture` became a real library holding the measured `wlcap` backend.
- `9253c37` — `composite_canvas` gained a `FrameSource` overload, so the scene walk can draw a
  frame it is handed without `lcomp` owning a device.
- `77cb1de` — the Vulkan device moved from `tools/spike-core` to `libs/lrhi`, where ADR-0005
  always said the GPU lives. Moved, not rewritten, so ADR-0011's measurements still describe it.
- `8accff2` — the two halves connected. Verified on this machine: RTX 2070, Hyprland, a real
  desktop in the Program panel.
- `6c54750` — removed six self-check assertions that were measuring event-loop passes rather
  than layout, and split the capture budget into a first-frame wait and a later-frame wait.

## What is in the uncommitted working tree

Everything here was written this session, builds clean under `-Werror`, and passes 24/24.

**Four operator-reported fixes:**

1. **The transform menu works on any source with a rectangle.** `placed_map` in
   `apps/libertas-ui/document.cpp` was listing drawable types by name, so only a colour or an
   image could be selected on the canvas. It now asks whether the node has a natural size. A
   screen's natural size is its monitor's resolution, read from the compositor and cached for
   half a second (`screens_now()`).
2. **Shift is inverted.** Aspect is locked by default; Shift frees it. A source is a picture
   with a shape of its own, and distorting it should be the gesture you ask for. Alt is
   unchanged. The self-check asserts both directions.
3. **Edge snapping, 12 screen pixels**, to canvas edges and centre lines. Screen pixels rather
   than canvas units so the feel does not change with zoom. Works with the aspect locked: the
   other axis is recomputed from the ratio rather than distorting.
4. **A screen dropdown.** `source.screen`'s `output` field is now a `choice` filled at runtime
   from `libertas::capture::list_screens()`. The UI asks the compositor directly — listing
   outputs is a registry read, not a capture, so it needs no GPU and no session.

**Two bugs found by the self-check while making those**, both mine, both fixed: Alt was
recentring per-axis so the rectangle drifted, and a snapped edge under Alt moved without its
opposite number (11px off centre).

**The settings-dialog preview for a screen** now shows the chosen monitor's connector,
resolution and refresh instead of "no pipeline attached". Not live pixels: that would open a
second capture session for a source that may be cancelled.

**The preview republishes on a timer while a live source is on the canvas.** It previously
published only on `Document::changed`, which is why capture produced one frame and then a still
picture. The rate is the canvas's own `fps`, not the monitor's. `Document::hasLiveSource()`
decides, and states the live types rather than the still ones so an unknown type from a later
build defaults to still.

**Six performance changes**, each one measured:

| Change | Why |
| --- | --- |
| Capture moved to its own thread, waiting unbounded | ADR-0013 Consequence 4: a bounded wait must never be used to sample. The old code waited 120 ms per tick and returned nothing — an 8 fps ceiling regardless of the screen |
| `CapturedPicture` borrows rather than owns | Two 33 MB copies per frame at 4K, 2.6 ms each, against 1.6 ms for the scaling they fed |
| BGRA→RGBA folded into the sampler (`swap_rb`) | A separate pass over the whole buffer measured 5.1 ms a frame at 4K, converting pixels that were about to be scaled away |
| `srgb_to_linear` replaced by an exact 256-entry table | `std::pow` per channel per pixel measured 110 ms on a 1080p frame. Exact, not approximate — the table is filled from the same function |
| Decoded images cached on path + mtime + size | A still was re-decoded every composite. Fine when composites only happened on edit; not at 60 fps |
| `sample_axis` precomputed per axis instead of per pixel | Two `idiv` instructions were 68% of the blit. 35 ms composite to 5.3 ms. See below |
| Composites skipped when no new frame and no document change | A 30 Hz screen on a 60 fps canvas has nothing new half the time |

## The problem that was open, and its answer

**It was integer division, and `perf` named it in one run.**

The composite took ~35 ms, of which the blit was 33 ms, while a faithful standalone
reproduction of the same blit ran in 1.16 ms. Six hypotheses had been advanced and eliminated
before a profiler was available. The profile settled it immediately:

```
93.75%  libertas-render  [.] lcomp::blit(...)
```

and annotating that symbol:

```
36.87 :  2f680:  idiv   %rbx
31.09 :  2f648:  idiv   %r9
 7.70 :  2f6b3:  movzbl 0x3(%r9),%eax
```

**Two `idiv` instructions, 68% of all samples.** `sample_axis` does a 64-bit integer division,
and the blit called it once per axis per pixel: at 1920x1080 that is 4.1 million divisions a
frame. 64-bit `idiv` is 20-40 cycles on this core and does not pipeline, so it dominated
everything else in the loop.

Why the standalone benchmark did not show it: the benchmark's inner loop was simple enough that
the compiler hoisted the `y` division out of it. The real blit could not, because the branch
choosing between the straight path and the rotate/flip cases kept both divisions inside.
**That is the lesson worth keeping — the benchmark was not wrong about the code, it was wrong
about the loop the compiler would build.**

### The fix

`axis_map()` in `libs/lcomp/src/composite.cpp` precomputes the source index for every
destination row and column, once per blit, using `sample_axis` itself so the arithmetic is
identical and the picture does not change. A 4K-to-1080p blit now does about 3000 divisions
instead of 4.1 million.

Built only for the straight path. A quarter turn reads the other axis of the source for each
destination axis, so its four cases would each need their own pair; they are left on the
per-pixel path until a profile says otherwise.

### What it bought, measured

| | before | after |
| --- | --- | --- |
| composite (walk) | 35.1 ms | 5.3 ms |
| of which blit | 33.0 ms | 4.9 ms |
| of which image source | 2.1 ms | 0.4 ms |
| render process CPU | 78% | 53% |

The image source got faster too, because it goes through the same blit.

### What is left, and whether it matters

A re-profile after the fix shows no single hotspot: the time is spread across the pixel loop
(the heaviest instruction is a 12% load of the source alpha byte), which is what a memory-bound
blit is supposed to look like. There is no second `idiv`-shaped mistake hiding behind the first.

53% CPU for 5.3 ms of work per composite is the composite running at the canvas's 60 fps while
the captured screen only produces 30. The freshness skip added this session should cover that
and does not appear to be firing as often as it should — **that is the next thing to look at,
and it is cheap**: `Screens::anything_new()` in `apps/libertas-render/main.cpp` and the
`document_changed` flag in `libs/lcomp/src/preview.cpp`.

### Method note

The six earlier hypotheses (memory copying, `std::pow`, image decode, cold GPU memory,
optimisation flags, the wrong `blit`) were all reasonable and all wrong. A profiler answered in
one run what reasoning had not in several hours. Two practical notes for next time:

- `pgrep -f libertas-render` matches the shell wrappers as well as the process. Use
  `pgrep -f 'build-make/apps/libertas-render'`, and check the pid is stable before recording.
- `timeout 15 perf record ...` SIGKILLs perf before it writes its data, giving "no samples".
  Use `perf record ... -- sleep 10`, which lets perf terminate normally.

## The next phase: the GPU path

This is the work after the composite is understood, and it should be chosen on its own merits
rather than as a fix for the open problem above. The operator's position, stated this session:
**the GPU route is being built regardless; CPU was never the goal except as the fallback for a
machine without a GPU — but making the CPU path as good as possible is still worthwhile.**

### What is already in place

- `libs/lrhi` owns a Vulkan device: allocation, dmabuf export and import, readback to host
  memory. Moved from the spikes with its measurements intact (ADR-0011, ADR-0019).
- `libs/lcapture` captures a screen into a ring of client-allocated dmabufs and can hand back
  either CPU pixels or — with work — leave them on the device.
- `libertas-render` already owns the device, which is where ADR-0005 always said the GPU lives.
- `tools/spike-core` still holds the measured encode half, including the BGRA→NV12 compute
  shader (ADR-0019), hooked to `lrhi`'s context through `VkCtx::on_destroy`.

### What the GPU phase has to decide

These are ADR questions, and ADR-0042 is the next number.

1. **Where the scene walk lives when there are two compositors.** ADR-0040's last consequence
   is explicit: "the software picture must not grow into a second compositor that disagrees with
   the GPU one. When `lrhi` exists, the scene walk is the part worth sharing." A blend mode, a
   rotation or a capture implemented in only one of them is that disagreement. The walk should
   produce a description of what to draw, with the CPU and GPU backends consuming it, rather
   than two walks that happen to agree today.
2. **What the CPU path is for, once the GPU path exists.** It is the fallback for a machine with
   no usable device, and it is also what the headless self-check drives — the check runs
   offscreen on a box that may have no GPU at all, and ADR-0040 made that a requirement. Both
   paths must therefore stay correct, which is the argument for one walk.
3. **Whether the preview and the encode share a composite.** ADR-0006 puts the encode target at
   scene-linear RGBA16F on a `VkDevice`; the preview is a panel-sized sRGB8 view. They are not
   the same buffer and should not pretend to be, but they may well be the same walk at different
   resolutions.
4. **Where the captured frame stops being a dmabuf.** Today it is read back to host memory for
   the CPU walk. On the GPU path it should stay on the device from capture through composite to
   encode, and the readback should happen only for the preview, at preview size, if at all.
5. **The dual-GPU case.** This machine has an Intel UHD 630 and the RTX 2070, which is exactly
   ADR-0019's subject. `vk_create_exportable` already carries a `host_memory` flag for the
   system-RAM round trip a second GPU needs. None of the current capture path is tested across
   two devices.

### The order that probably makes sense

1. ~~Understand the 35 ms first.~~ Done: it was integer division in the sampler, fixed, and the
   CPU composite is now 5.3 ms. Nothing about it implicates the GPU path.
2. Write ADR-0042 for the shared scene walk, because it constrains everything after it.
3. Keep the frame on the device: capture → composite → preview readback, with the encode path
   still absent.
4. Only then the encode half, which already has measured code waiting in `tools/spike-core`.

## Things deliberately left undone, with reasons

- **`showCursor` and `pauseWhenHidden` are stored and ignored.** The cursor is always captured;
  nothing stops capturing when a source is off screen.
- **Nothing checks panel geometry in the scene graph.** Six assertions did and were removed in
  `6c54750` — they measured event-loop passes, not layout. `test_layout`, `test_dropzone` and
  `test_layout_serialize` cover the engine without a window.
- **The first-frame and later-frame capture budgets (3 s and 120 ms) are guesses from one
  machine.** The slow case is the untested one. Note that the later-frame budget is now moot on
  the threaded path, which waits unbounded; the first-frame wait still matters.
- **No portal backend in `lcapture`.** `linux-portal/` is still an empty directory; the portal
  code remains in `tools/spike-core`. It stays the fallback (ADR-0011) and is what would fill
  `output` in from the picker's `mapping_id`.
- **What happens when a named connector is gone** is undecided (ADR-0041). It is a running-show
  failure and belongs with a supervisor that does not exist yet.
