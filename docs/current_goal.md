<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Where the work is, and what is next

## Source expansion and performance pass, 2026-10-19

The GPU, render statistics and Elgato work below is the validated baseline for this pass.
The warning-free `-Werror` build and all 26 tests passed again before the baseline commit.
Next: measure and reduce render costs, finish Linux source backends and their transform menus,
then import OBS scene collections. Commit and push each validated section. macOS-only sources
are excluded at the operator's request; use Linux equivalents where available.

### Measured capture optimization

`perf` identified swscale's horizontal scaler as the dominant capture cost. At 2560x1440,
an isolated 300-frame conversion measured NV12-to-BGRA at 7.80 ms, planar YUV420-to-BGRA
at 1.19 ms, and BGR24-to-BGRA at 0.80 ms. Prefer YUV420, retaining the same USB bandwidth
as NV12 and preserving NV12 as a fallback. The real Elgato negotiated YU12 and sustained
59.39 fps with immutable leased frames; live renderer cycle samples over ten seconds fell
from 25.6 billion to 10.9 billion. These include capture and rendering and are not latency.

The repeatable `LIBERTAS_TEST_GPU=1 LIBERTAS_BENCHMARK=1` GPU test measures 1440p host upload,
4K composite and 640-edge preview readback: 7.32 ms/frame while the live show is also running.
A guarded 32-bit shader division experiment measured 7.18 ms, insufficient evidence to keep
extra branching; exact existing shader arithmetic remains. Larger gains need profiling the
host upload path separately from the GPU-only screen path.

### Native visual sources

ADR-0045 adds the isolated `libertas-source` worker. Text uses Qt font shaping and rasterization;
slideshows decode an ordered list of images; media and playlists use Qt's FFmpeg backend;
browser pages use sandboxed Qt WebEngine. The worker publishes straight RGBA through the existing
preview transport, and both compositors consume the same source snapshots. Explicit output
dimensions drive editor handles and placement. Text and playlist fields support multiple lines.

Worker checks assert real glyph pixels and transparency, changing slides, decoded moving video
and local HTML colours. CPU and GPU renderer-process checks verify text placement. Browser
rendering currently uses software frames and a host upload; profile before adding GPU sharing.
Sources restart when reactivated. Browser and media audio routing is not yet implemented.
Qt Multimedia and Qt WebEngine development packages are additional build requirements.

## Video capture update, 2026-10-18

ADR-0044 adds `source.v4l2` capture through native V4L2 streaming and libswscale BGRA
conversion. Add **Video Capture**, select a device, resolution and frame rate. Stable
device paths exclude metadata nodes. Explicit modes fail instead of silently downgrading.
CPU and GPU consume the same leased snapshot through the shared scene walk; duplicate
placements cannot observe different capture frames within a render commit.

The attached Elgato 4K X negotiated **2560x1440 at 60/1 fps**. The real capture-thread check
measured **59.39 fps over 181 frames**, including BGRA conversion, while retaining an older
frame and verifying it remained immutable. Real renderer-process checks passed with both
CPU and RTX 2070 GPU backends, including direct and nested placements. These measurements
describe acquisition, not end-to-end latency or sustained 4K canvas throughput.
The build is warning-free with `LIBERTAS_WERROR=ON`; all 26 CTest entries pass with
`LIBERTAS_TEST_GPU=1`, including the headless UI check.

This slice supports raw SDR NV12, YUYV, YUV420 and RGB/BGR24. Audio, compressed-only devices,
HDR, automatic reconnect and USB zero-copy remain unimplemented. libswscale development
files are now a build dependency. Hardware checks run sequentially because capture is exclusive:

```sh
LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_video
LIBERTAS_RENDER_BACKEND=cpu LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_preview
LIBERTAS_RENDER_BACKEND=gpu LIBERTAS_TEST_VIDEO=/dev/video2 build-make/tests/unit/test_preview
```

## GPU render path update, 2026-10-18

ADR-0042 and its implementation were developed from `main` at `57ce881` and are included
in the validated baseline described above. The earlier clean-tree statement below describes
the CPU handoff. `AGENTS.md` was already untracked before the GPU work.

- `lcomp::plan_canvas` is the one scene walk. CPU and Vulkan execute its resolved operations;
  transparent precomps, source availability, gaps, placement and ordering are shared.
- `lrhi::GpuCompositor` composites at canvas size in scene-linear RGBA16F. A separate GPU pass
  produces sRGB8 at the requested preview size; only that reduced buffer is read back.
- Capture publishes leased dmabuf slots. A render commit holds one snapshot per connector
  across repeated placements, nested scenes and canvases. Capture threads do no Vulkan work.
- Preview protocol v2 carries a maximum preview edge. QML reports its physical display size;
  refresh-only commits reuse the document and unchanged frames receive only an Ack. The UI
  retains its last image on that Ack. This also fixes the old freshness skip: timer ticks used
  to resend the document and mark it changed every time.
- Offscreen runs use CPU. Normal runs try Vulkan, log failure, and fall back to CPU. Set
  `LIBERTAS_RENDER_BACKEND=cpu` or `gpu` to force a backend; `gpu` fails instead of silently
  falling back. `LIBERTAS_RENDER_DEVICE=226:128` selects by DRM device identity (`226:129` is
  Intel here). Cross-device capture is explicitly refused until validated.

### Evidence and checks

The original suite passed 24/24 before changes. The expanded suite passes 25/25 when hardware
checks are enabled with `LIBERTAS_TEST_GPU=1`; without that variable the GPU test is skipped.
The real process check also runs with a required GPU. Hardware parity checks pass on both RTX
2070 and Intel UHD 630, including rotations, flips, opacity, nested scenes and reduced preview.
CPU retains its sRGB8 intermediate rounding; GPU retains half-float linear intermediates. The
tested scenes allow at most two byte values of channel error, not a universal bound for an
arbitrary number of translucent layers.

Real captures on HDMI-A-1 and HDMI-A-2 pass pixel comparison and Vulkan synchronization
validation. Repeated rendering of one captured frame, including a 640-edge preview readback,
measured **1.30 ms at 2560x1440** and **1.87 ms at 3840x2160**, averaged over 60 frames.
The final 4K run with producer-fence waiting and synchronization validation measured 1.67 ms.
These are composite/readback timings, **not sustained capture throughput**. A separate real
process check keeps capture running and verifies direct and repeated nested placements read
identical snapshots. An unavailable DRM device also passes the CPU fallback process check.

```sh
cmake --build build-make -j 8
LIBERTAS_TEST_GPU=1 ctest --test-dir build-make --output-on-failure
LIBERTAS_RENDER_BACKEND=gpu LIBERTAS_TEST_CAPTURE=HDMI-A-2 build-make/tests/unit/test_preview
LIBERTAS_TEST_GPU=1 LIBERTAS_TEST_CAPTURE=HDMI-A-2 build-make/tests/unit/test_gpu_composite
LIBERTAS_TEST_GPU=1 LIBERTAS_RENDER_DEVICE=226:129 build-make/tests/unit/test_gpu_composite
```

### What follows this slice

Encode/output integration is still absent. The full-size linear target stays in the render
process, but an encoder handoff API is not exposed yet. ADR-0019's converted host-memory
transfer to a secondary encoder still needs integration and measurement. Cross-device capture
is rejected rather than assumed to work. A timed-out GPU fence terminates the render process
without recycling potentially live resources; automatic device-loss recovery still belongs to
the future supervisor/output work. The decoded-image upload path currently uploads each drawn
image per composite; profile a real image-heavy show before adding a texture cache.

## Previous CPU handoff

Written 2026-10-18, at the end of a session that got a real screen drawing in the Program panel
and then went looking for why it is slow. Read this before picking the work up elsewhere.

## The state of the tree right now

`main` is at `8731e1c`, pushed, **and the working tree is clean**. Everything this session
produced is committed: the benchmark instrumentation has been stripped, the build is
warning-free under `-Werror`, and the suite is 24/24.

The app was launched after the final commit to confirm capture still opens and the composite
idles at 0% CPU on a static screen.

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
- `ee69359` — the blit was doing four million integer divisions a frame. See "The problem that
  was open" below.
- `cb03a51` — this document, with the CPU cost table and the GPU brief.
- `8731e1c` — the four operator-reported fixes, the screen dropdown, the live preview timer and
  the performance work. The section below is its shape.

## What landed in the last commit (`8731e1c`)

All committed. Listed because the commit is large and this is the shape of it.

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

## What the CPU path costs, measured 2026-10-18

Run on this machine: RTX 2070, Hyprland, capturing HDMI-A-2 (3840x2160 @ 30 Hz), one screen
source plus one image source in the program scene.

| | 1080p canvas (4K downscaled) | 4K canvas (1:1) |
| --- | --- | --- |
| alloc + clear | 0.78 ms | 3.3 ms |
| blit | 4.9 ms | 18.4 ms |
| total walk | 5.3 ms | 21.4 ms |
| render CPU | 53% | 53% |
| sustained | comfortable at 30 | **~10 fps** |

A 1080p preview of a 4K screen is fine: 5.3 ms a composite leaves plenty of room at 30 Hz, and
the CPU figure is the composite running faster than the source produces rather than the blit
struggling.

Compositing 4K one-to-one is **~10 fps**, and that is the honest ceiling of this path. The blit
is 18.4 ms for 8.3 million pixels, which is about 2.2 ns a pixel -- memory-bound, no single hot
instruction left (the post-fix profile is 46% blit with the time spread across the loop rather
than concentrated). It is not a bug to be found; it is what a single-threaded scalar copy of
33 MB per frame costs.

The operator's position on this, recorded so it is not relitigated: **4K on an integrated GPU or
a CPU fallback at 10 fps is acceptable.** This path exists for machines without a usable device
and for the headless self-check. It is not the path a show runs on.

### What would make the CPU path faster, if it ever needs to be

Not done, and not currently justified. Listed so the options are known rather than rediscovered:

- **Thread the blit by destination rows.** It is embarrassingly parallel and there are 16 cores.
  Probably 4-6x on this machine.
- **SIMD the inner copy.** The straight path moves four bytes with a channel swap; that is a
  shuffle, and at 8.3 million pixels it would matter.
- **Composite at preview resolution, not canvas resolution.** The Program panel is a few hundred
  pixels tall. Compositing 4K to show it in a 400px box is most of the cost, and the encode path
  will want the full-size composite anyway -- so this is really "the preview should ask for the
  size it will draw".

The third is the one that would help a real show, and it belongs with the GPU work rather than
before it.

## The next phase: the GPU path

**This is the work to start next.** The CPU path is now as good as it needs to be for what it is
(see the section above), and the GPU path was always the intended one -- the operator's words:
*the GPU route is being built regardless; CPU was never the goal except as the fallback for a
machine without a GPU.*

### Where to start reading

- `docs/adr/0005` — six process roles. `libertas-render` owns the GPU. This is the ADR that
  settles where the device lives, and it has said so since the start.
- `docs/adr/0006` — the composite-to-encode handoff: scene-linear RGBA16F on a `VkDevice`.
- `docs/adr/0040` — why the preview is a separate process, and its last consequence, which is
  the constraint this phase has to respect.
- `docs/adr/0011`, `0019` — what was measured about capture and dual-GPU encode. The code those
  numbers describe is now in `libs/lrhi` and `libs/lcapture`, moved rather than rewritten.

### What is already built and working

- `libs/lrhi` owns a Vulkan device: instance and device creation, dmabuf export and import,
  readback to host memory, and a BGRA→NV12 compute pass hooked in from the encode spike.
- `libs/lcapture` captures a named screen into a ring of client-allocated dmabufs, on its own
  thread, blocking until the compositor delivers. Verified: 3840x2160 AR24 on an RTX 2070.
- `libertas-render` already opens the device, lazily, on the first screen source.
- `libs/lcomp` walks the scene and composites on the CPU, which is the fallback and the thing
  the headless self-check drives.

### The constraint that shapes everything

ADR-0040's last consequence, verbatim:

> The software picture must not grow into a second compositor that disagrees with the GPU one.
> When `lrhi` exists, the scene walk is the part worth sharing. A blend mode, a rotation or a
> capture implemented only here would be the disagreement.

`lrhi` now exists. So the first real decision is **not** "how do we draw on the GPU" but **"what
do the two backends share"**. The likely shape: the walk produces a list of draw operations --
source, rect, opacity, quarter turn, flips -- and the CPU and GPU backends each consume that
list. Two walks that happen to agree today will not agree in six months.

### The decisions ADR-0042 has to make

1. **The shared walk.** What the walk emits, and what a backend is. This constrains everything
   after it, so it comes first.
2. **Where the captured frame stops being a dmabuf.** Today it is read back to host memory for
   the CPU walk. On the GPU path it should stay on the device from capture through composite to
   encode, and the readback should happen only for the preview, at preview size.
3. **Preview size versus canvas size.** The Program panel is a few hundred pixels tall and the
   composite is currently full canvas resolution. The encode path wants full size; the preview
   does not. Deciding this well is most of the CPU path's remaining cost too.
4. **What happens with no usable device.** The CPU path stays, because the self-check runs
   offscreen and some machines have no GPU. Both paths must stay correct, which is the argument
   for one walk.
5. **The dual-GPU case.** This machine has an Intel UHD 630 alongside the RTX 2070 — exactly
   ADR-0019's subject. `vk_create_exportable` already carries a `host_memory` flag for the
   system-RAM round trip a second GPU needs. None of the current capture path is tested across
   two devices.

### A suggested order

1. Write ADR-0042 for the shared walk. Code after, per the repo rule.
2. Keep the frame on the device: capture → GPU composite → readback at preview size only.
3. Then the encode half, which has measured code waiting in `tools/spike-core`.

### Before writing any code, read this

The repo rules that bit hardest this session:

- **Model changes need an ADR before code.** ADR-0042 is next.
- **Non-trivial behaviour gets a self-check assertion that drives the real thing**, not the
  function behind it. The GPU path cannot be driven by the offscreen check, which is exactly why
  the CPU path has to stay correct and checked.
- **Builds are warning-free under `-Werror` and the suite is green before a commit.**
- **Measure before changing.** Six reasoned hypotheses about the composite were all wrong this
  session; one `perf` run found the answer. `perf` is installed now.

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
- **"Fit to screen" bakes a layout box at the canvas size it was used on.** Changing the canvas
  afterwards leaves the node drawing at the old size, because `fit: natural` then resolves
  against the stored box rather than the source. Hit while benchmarking: a canvas changed to 4K
  kept compositing its source at 1920x1080, which looked like a bug in the canvas resize and was
  not. Whether a fit should re-resolve when the canvas changes is a real question and nobody has
  answered it.
