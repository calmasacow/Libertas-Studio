<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Where the work is, and what is next

Written 2026-10-18, at the end of a session that got a real screen drawing in the Program panel
and then went looking for why it is slow. Read this before picking the work up elsewhere.

## The state of the tree right now

`main` is at `6c54750`, pushed. **There are uncommitted changes in the working tree** — about
eighteen files. They build warning-free and the suite is 24/24, but they are not committed
because the thing they were meant to fix is not fixed. See "The open problem" below before
deciding what to do with them.

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
| Composites skipped when no new frame and no document change | A 30 Hz screen on a 60 fps canvas has nothing new half the time |

## The open problem

**The composite takes ~35 ms and I cannot explain it.**

With `DIAG` instrumentation in place, on a 1920x1080 canvas drawing a 3840x2160 screen capture
plus one image source:

```
DIAG alloc+clear 0.79  walk 35.10  (image 2.11  screen 32.99  fetch 0.00  blit 32.98)
```

So: allocation and clear are 0.8 ms, the image source is 2.1 ms, fetching the captured frame is
free, and **the blit is 33 ms**.

That blit is `blit()` in `libs/lcomp/src/composite.cpp`, scaling 3840x2160 down to 1920x1080
with `swap_rb` on. Its geometry was logged to be certain: `src 3840x2160 -> rect 0,0..1920,1080
(1920x1080) op=1 q=0`, and the source alpha was probed and is 255, so it takes the opaque fast
path and never touches the blend.

**A faithful standalone reproduction of that same loop runs in 1.16 ms.** Same dimensions, same
`-O2` (confirmed from `build-make/libs/lcomp/CMakeFiles/lcomp.dir/flags.make`), same
`sample_axis` with its clamp, same borrowed-pointer indirection, same channel swap, alpha 255
throughout. The benchmark source is `/tmp/b6.cpp` if it survives; it is small enough to rewrite.

A 28× gap, unexplained.

### Hypotheses already tested and eliminated

Do not spend time re-testing these.

- **Memory copying.** Removed both 33 MB copies. No change.
- **`std::pow` in the colour conversion.** Replaced with an exact lookup table. No change —
  and the probe showing alpha 255 explains why: the blend path was never being entered.
- **Re-decoding the PNG every frame.** Cached. Saved about 2 ms, which shows in the `image
  2.11` figure, but did not touch the 33 ms.
- **Cold GPU memory.** `vk_readback_exported` memcpys into an ordinary `std::vector`, so the
  buffer the blit reads is normal heap memory, not mapped device memory.
- **Optimisation flags.** `-O2 -g -DNDEBUG`, same as the benchmark.
- **The wrong `blit` being called.** There is only one; the other function at a similar line
  number is `fill_rect`.

### What to do next

`perf` has now been installed, which is what this needed all along. One profile of
`libertas-render` under load should name the function:

```sh
# with the app running and a screen source on the canvas
perf record -F 299 -g -p "$(pgrep -f libertas-render | head -1)" -o /tmp/r.data -- sleep 8
perf report -i /tmp/r.data --stdio --no-children | head -40
```

The build already carries `-g` and `-fno-omit-frame-pointer`, so the stacks should be readable
without rebuilding.

Treat the 1.16 ms benchmark as the thing to explain, not as proof the code is fine. The
difference is real and reproducible; the explanation is simply not yet found.

### A caution for whoever picks this up

Four hypotheses were advanced confidently in this session and all four were wrong. Two readings
of which process was hot were also reported backwards, because `ps` snapshots were trusted over
`/proc` deltas. Measure before changing anything, and prefer:

```sh
read a b < <(awk '{print $14, $15}' /proc/PID/stat); sleep 5
read c d < <(awk '{print $14, $15}' /proc/PID/stat); echo $(( ((c+d)-(a+b))*100/500 ))%
```

over a `ps %cpu` snapshot, which reports a lifetime average.

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

1. Understand the 35 ms first. If the CPU blit is pathological for a reason that also affects a
   GPU upload path, it is better known now.
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
