<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0019 — OBS's dual-GPU encode round-trips through system RAM, and converts before readback

- **Status:** accepted, observed live
- **Date:** 2026-10-12
- **Confirms:** ADR-0007 (dual-GPU concurrent encode), ADR-0012 (cross-GPU transfer)
- **Improves:** ADR-0018 (the software fallback tier)

## What was observed

Measured on argus while the author was **actually live** - YouTube via NVENC at 1440p, a local
recording, and Twitch via QuickSync at 1080p, the exact dual-GPU configuration ADR-0007 is built
for. The author's own OBS Stats dock, before and after starting the Twitch output:

```
                             stream + recording     + Twitch (QSV)
average time to render frame        3.9 ms               8.0 ms
OBS CPU usage                       6.7 %                12 %
```

**Adding one 1080p output to an idle iGPU doubled the render time of the whole mix.** That is not
what adding a second hardware encoder should cost, and the OBS log says why:

```
NV12 texture support enabled
>>> app not on intel GPU, fall back to old qsv encoder
[qsv encoder: '... Twitch Output Main'] settings:
        width: 1920   height: 1080   target_bitrate: 8000
[qsv encoder: '... Twitch Output Main'] debug info:
        surf:           SysMem
```

`surf: SysMem`. The QuickSync texture path requires the application's render device to *be* the
Intel GPU. OBS composites on the NVIDIA GPU, so there is no shared surface to hand over, and it
falls back to the encoder's raw path. That flips `raw_active` true in `obs-video.c`, which
re-enables `download_frame()` - so the **entire mix is now read back to system RAM every frame**,
purely so the frame can be DMA'd straight back out to the iGPU.

The frame crosses PCIe twice to reach a device that was already on the bus. This is precisely the
problem ADR-0012 set out to measure, and OBS's answer to it is a full host round trip.

## The A/B, run live

The author then moved the Twitch output from QuickSync to a second NVENC session, which keeps it
on the texture path, and the readback disappeared:

```
                           Twitch on QSV      Twitch on NVENC
NVENC sessions                   1                  2
libobs:graphics thread        34.1 %             24.4 %
OBS process total            191 %              162.8 %
NVIDIA GPU utilisation        80 %               78 %
```

The graphics-thread drop is the signature of `download_frame()` no longer running: the second
output stopped charging the first one for its existence. (`top` samples over two seconds, so the
direction is solid and the last digit is not.)

The GPU row is the point worth keeping. **Adding an entire second 1080p60 encode changed NVIDIA
GPU utilisation by nothing** - because NVENC is dedicated silicon, not shader cores. A second
encode genuinely is close to free when it is not made to travel through system RAM to get there.
The 2x render-time penalty was never the cost of encoding; it was the cost of the detour.

Nor does the second session contend for the encode engine in any way that shows up. NVENC's own
reported latency, sampled while both sessions ran, was **7713 / 7260 / 8255 us** against
**7971 us** when only the 1440p session existed. A first reading of 9563 us looked like
contention and was a transient; three samples say there is none. The RTX 2070 has a single NVENC
engine and is carrying 1440p60 and 1080p60 on it concurrently for free.

This is the Phase-1.5 bar, now with both a failing and a passing example measured on the same
machine on the same evening.

**This is not an argument that NVENC beats QuickSync, nor that one GPU beats two.** The author's
original reasoning - offload the secondary stream onto otherwise idle Intel silicon - is sound,
and remains the configuration this project intends to make *correct* rather than to replace. What
the A/B shows is that the incumbent cannot route a frame to the second GPU without a host round
trip, so on OBS today the two-chip setup is the slower one. Fixing that is ADR-0007's whole
purpose; when it works, spreading the load across two engines is better again, and matters more
on a machine with less NVENC headroom than this one.

## Why this matters more than any other finding so far

The author's stated reason for this project is years of trouble with OBS on Linux, and their
daily configuration is the one that triggers this. Two of our existing decisions are now validated
by a live measurement rather than by argument:

- **ADR-0007** (dual-GPU concurrent encode as a first-class feature, each VAAPI encoder in its own
  process) is not a nice-to-have; the incumbent degrades the *primary* output to add a secondary
  one.
- **ADR-0012** (the shared buffer is a dmabuf allocated `HOST_VISIBLE` and **not** `DEVICE_LOCAL`,
  scaled on the source GPU before transfer) is the fix. We allocate the transfer buffer ourselves
  and hand the Intel device a dmabuf it can import directly. Nothing is copied into a generic
  sysmem surface pool, no CPU reads a pixel, and scaling to 1080p before the transfer cuts the
  bytes by 44% (ADR-0012). Our second encode should cost the first encode **nothing**.

That last sentence is the promise this ADR exists to pin down, and it is the one to test at
Phase 1.5: *starting the second output must not change the first output's frame time.* OBS fails
that test by 2x. It is a concrete, measurable, user-visible bar.

## Second finding: OBS converts on the GPU *before* staging, and we do not

From `stage_output_texture` in `obs-video.c`, when `gpu_conversion` is set (the default), OBS
stages `convert_textures[i]` - the **already-converted NV12 planes** - rather than the RGBA output
texture. So even on its slow path OBS:

- reads back **1.5 bytes/pixel** (NV12), not 4 (BGRA): 5.3 MiB at 1440p rather than 14.1 MiB;
- pays **nothing** on the CPU for the colour conversion, because a shader did it.

Our `encode-spike` does both the other way round: it reads back BGRA and converts with `sws_scale`
on the CPU. ADR-0018's corrected measurements say exactly what that costs:

```
readback      1.870 ms     (14.06 MiB of BGRA)
bgra->nv12    4.351 ms     (on the CPU)
encode        0.500 ms
              --------
              6.721 ms
```

Converting in a shader before readback should remove the 4.351 ms outright and cut the readback by
2.7x, to roughly **1.2 ms total**. That is a ~5x improvement to the fallback tier using only
machinery we already have, and it should land before the Vulkan tier, not after it - it is
cheaper to build and it benefits every machine that can never reach the Vulkan tier at all.

ADR-0018 called the GPU colour conversion "the largest single software cost"; this is the evidence
for the order it should be done in.

## Third observation: media sources decode on the CPU

The per-thread breakdown of the live OBS process (191% CPU total) shows the cost is not in one
place:

```
libobs:graphics   34%     compositing + the readback above
PipeWire          29%     audio
mp_media x6     5-10% ea  video file sources, decoding on the CPU
v4l2: capture   5-10%     the Elgato
cuda-Ev...         5%     NVENC submission
```

The author's heaviest scene holds six-plus looping video files (`ffmpeg_source`), each decoding on
the CPU. Hardware decode for media sources is therefore a real differentiator and not a micro
optimisation - on this scene it is a larger CPU cost than compositing. Recorded here as an input
to Phase 2, not a decision.

## Consequences

- Phase 1.5 gains an explicit acceptance test: **enabling the second encode must not measurably
  change the first encode's frame time.** Numbers above are the incumbent's score on that test.
- The fallback tier's colour conversion moves to a shader, scheduled **before** the Vulkan tier.
- `vk_create_exportable`'s `host_memory` path (ADR-0012) is the mechanism for the hand-off and is
  already proven; what remains open there is a P010/NV12 transfer buffer rather than BGRA, which
  this ADR makes the priority question.
- Hardware decode for media sources is logged as a Phase-2 candidate with a measured reason.
