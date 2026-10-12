<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0013 — Capture is damage-driven; the encoder needs its own clock

- **Status:** accepted, measured
- **Date:** 2026-10-12
- **Closes:** the throughput question left open by ADR-0012

## What was measured, and a correction

A first throughput run reported `mean 9.03 ms (110.8 fps), p50 6.07, p99 36.75, 72/600 frames over
the 60 fps budget`, and that tail was read as a latency problem worth fixing. **That reading was
wrong**, and so was the first fix attempted for it.

Adding a buffer ring (2, 4, 8 slots in place of 1) changed nothing. Re-running on a quieter desktop
made every configuration *worse*, which is the opposite of what a serialization bottleneck does.
The missing instrument was the compositor's own `presentation_time`:

```
  per frame     : mean 26.56 ms (37.7 fps)  min 2.15  p50 30.28  p99 39.64  max 40.33
  presented gap : mean 26.64 ms (37.5 fps)  p50 30.29  p99 39.64
```

Inter-arrival time and the compositor's presented-frame interval agree to within 0.1 ms, across
every ring size and every run. **The capture path adds no measurable latency over presentation.**
What varies is how often the screen changes: ~110 fps with a video playing on a 165 Hz output,
~33 fps on a largely static desktop. The "tail" was the desktop not changing.

The lesson is methodological and general: *inter-arrival time cannot measure a capture path on a
damage-driven protocol.* It measures the content. The instrument must be the producer's own
timestamp, and that instrument should have been in place before any number was reported.

## Consequence 1: this is correct behaviour, and we must not "fix" it

`ext-image-copy-capture-v1` delivers a frame when the output is damaged. Burning GPU time copying
an unchanged screen 60 times a second would be strictly worse. So the capture source is inherently
variable-rate, and **the output cadence is the encoder's responsibility, not the capture worker's**:

- `libertas-render` owns one monotonic clock and drives encode at the output's configured rate.
- When no new frame has arrived by the deadline, the compositor pipeline re-presents the last good
  frame. For an unchanged screen the encoder sees an identical frame and emits a cheap P-frame, so
  a static scene costs almost nothing in bitrate - which is the behaviour viewers expect.
- This is the same mechanism already required for a *stalled* worker (ADR-0005's last-good-frame
  rule), so it is one path, not two.

## Consequence 2: capture rate and output rate are independent, and must stay so

The reference setup makes this concrete. The author's primary display runs at **165 Hz**, and when
an Elgato capture card is attached it is fed a **1440p mirror at 119.88 Hz** while the show encodes
at **59.94 fps**. Three different rates in one pipeline, none of them a multiple of the encode rate.

So frame selection is resampling, and it must be explicit:

- Timestamp every captured frame with the compositor's `presentation_time`, not arrival time.
  Arrival time includes our own scheduling jitter and is not a property of the frame.
- The encoder picks, for each output period, the most recent frame whose presentation timestamp
  precedes the deadline. Dropping surplus frames from a 165 Hz source to a 59.94 fps output is
  normal operation, not an error, and must not be logged as frame loss.
- Never derive the output rate from the capture rate. A 165 Hz source and a 59.94 fps output share
  no common beat, so any attempt to lock them produces visible judder.
- **Audio remains the master clock** (unchanged from the plan). Video resamples to it.

## Consequence 3: keep the buffer ring anyway

The ring made no difference here, and the honest reason is that the capture path was never the
bottleneck in this test. It stays, because in the real worker a slot is held while the frame is
composited, scaled, transferred across PCIe and encoded - work this spike does not do. A
single-buffer design would then genuinely serialize the compositor against the encoder. The ring is
retained on that argument, not on a measurement, and that distinction is recorded rather than
blurred.

## Still open

- The ceiling under sustained synthetic damage was not isolated: the attempt to force constant
  damage with `vkcube` put the window on a different output and the measurement was unchanged. A
  controlled damage source on a *named* output is needed before quoting a maximum capture rate.
  Until then the honest claim is "adds no measurable latency over presentation", not a fps number.
- Per-frame cost of the cross-GPU transfer at the output cadence (ADR-0012 still open).
- Explicit sync via `linux_drm_syncobj_v1`; the spike still stalls on a fence.
