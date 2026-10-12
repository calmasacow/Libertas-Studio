<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0014 — Capture cards: pick the format for the output rate, not the peak rate

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Extends:** ADR-0013 (capture is damage-driven)
- **Hardware:** Elgato 4K X on `argus`, fed from the dev box's HDMI-A-1

## Context

A capture card is the second kind of video source, and it behaves nothing like the screen-capture
backend. ADR-0013 established that `ext-image-copy-capture-v1` is damage-driven and therefore
variable-rate. A card is the opposite: it has its own crystal and produces frames whether or not
anything changed. Everything below was measured rather than assumed, because the card's own
reporting is unreliable — it presents as a generic UVC camera, reports input `Status: ok` with no
cable attached, and does not implement `QUERY_DV_TIMINGS`.

The signal on the wire during these measurements: **2560x1440 @ 120.171 Hz, 8-bit sRGB RGB
(`XRGB8888`), HDR disabled**, mirroring DP-1.

## Finding 1: only one format exceeds 60 fps at 1440p

`VIDIOC_ENUM_FRAMEINTERVALS` per pixel format at 2560x1440:

| format | 4:4:4? | range | max rate offered |
|---|---|---|---|
| `NV12` 4:2:0 8-bit | no | limited | **144 / 120** / 60 / 59.94 / 50 / 30 |
| `BGR3` 24-bit BGR | **yes** | **full** | 60 / 59.94 / 50 |
| `YUYV` 4:2:2 | no | — | 60 / 59.94 / 50 |
| `P010` 4:2:0 10-bit | no | — | 60 / 59.94 / 50 |

So the high frame rates are bought entirely with colour. There is no 1440p120 path that is not
4:2:0, and no 10-bit path above 60.

## Finding 2: the card converts, and the conversion is lossy twice over

The source sends full-range 4:4:4 sRGB RGB. What comes back depends on the format asked for, and
the difference is directly visible in the sample statistics:

```
NV12 @120 : luma (plane 0): mean 82.58  min 16  max 229   -> limited range
BGR3 @60  : plane 0       : mean 83.53  min  0  max 240   -> full range
```

`min 16` versus `min 0` is the whole story. In NV12 the card has compressed full-range RGB into
limited-range Rec.709 **and** thrown away three quarters of the chroma. For a desktop source that
is the worst possible trade: 4:2:0 subsampling lands directly on text edges, and the range
compression discards code values that then have to be expanded again downstream.

Note also that V4L2 reports this stream as `Colorspace: sRGB` with `Transfer Function: Rec. 709` —
two different transfer curves named in one format description. The card's own metadata cannot be
trusted to drive colour conversion; the pipeline must be told what the source is.

## Finding 3: three clocks, and a drop no counter can see

Measured over 60 s / 7200 frames of NV12 @120:

```
driver gap    : mean 8.334 ms (119.99 fps)  p50 8.333  p99 8.411  max 11.145
measured rate : 119.9860 fps over 60.00 s
driver seq    : 0..7199, 0 break(s), 0 frame(s) never delivered to us
```

The card does not genlock. It free-runs at **119.986 fps** against a source presenting
**120.171 Hz** and samples whatever is on the wire. The capture path itself is clean — zero
sequence breaks over 7200 frames means we never once fell behind the driver.

But the two rates differ by 0.185 fps, so roughly **one source frame in every 650 is never
sampled** — one every 5.4 s, about 5300 over an eight-hour show. And `driver seq` reports no gaps
at all, because it counts frames the **card** produced, not frames the **source** sent. The loss
happens upstream of the counter entirely.

This extends ADR-0013 rather than contradicting it. There, the lesson was that inter-arrival time
measures content, so trust the producer's timestamp. Here, even the producer's own counter is
blind, because the producer is not the origin. Confirming such a drop directly requires a counter
rendered into the source image; it cannot be observed from inside V4L2.

## Finding 4: a reproducible ~680 ms stall when asking for 144

```
run 1: p50 6.944  p99 7.287  max 669.167   -> 123.99 fps average
run 2: p50 6.944  p99 6.966  max 682.405   -> 129.94 fps average
```

`p50 6.944 ms` is exactly 144.000 fps, and with the single stall excluded the card sustains it.
The stall reproduces run to run and drags the average down by 14-20 fps. A capture worker must
therefore treat a multi-hundred-millisecond gap at stream start as normal rather than as a hang,
and must never let it stall the show.

Separately, startup black is **2-4 frames** of `Y=0` before real content, and the count varies run
to run. The settle window is a measurement, not a constant — and no signal verdict may be taken on
frame 0.

## Decision

**Select the capture format from the configured output rate, not from the card's peak capability.**

For this show, which encodes 1440p60 to YouTube and 1080p60 to Twitch, that means **`BGR3` @ 60**:

```
driver gap    : mean 16.667 ms (60.00 fps)  p50 16.666  p99 16.771  max 16.797
measured rate : 60.0008 fps   driver seq: 0..299, 0 break(s)
dmabuf export : yes (zero-copy possible)
SIGNAL        : PRESENT (full range)
```

Full-range 4:4:4 RGB, no chroma loss, no range round-trip, a 16.797 ms worst case over 300 frames,
and `VIDIOC_EXPBUF` works — so the zero-copy path is available here too, not only for NV12.
Choosing NV12 @120 would cost all of that colour fidelity to deliver frames that get decimated to
60 before they are ever encoded.

The general rule: **capturing above the output rate is only worth its colour cost when the extra
frames survive to the output.** They do not here.

Consequences for the implementation:

1. The V4L2 source negotiates format against the *output* configuration, and the UI must surface
   the trade (`1440p120 4:2:0 limited` vs `1440p60 4:4:4 full`) rather than silently maximising
   frame rate.
2. Range and matrix are **configuration, not detection.** The card's metadata is self-contradictory.
3. Every source carries its own clock. Neither the card's nor the compositor's is the encoder's,
   and audio remains the master clock.
4. A rate mismatch between source and card is silent and unobservable from V4L2. Where it matters,
   match the output mode to the capture rate at the *source* — driving HDMI-A-1 at 60 Hz rather
   than 120.171 Hz removes the drift rather than compensating for it.
5. Capture-card sources are fixed-rate and must not reuse the damage-driven assumptions of
   ADR-0013.
