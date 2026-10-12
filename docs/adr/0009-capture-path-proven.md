<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# ADR 0009 — Capture path: portal -> PipeWire -> dmabuf works; the negotiation protocol in full

- **Status:** accepted (stages 1-2 of the capture spike)
- **Date:** 2026-10-11

## Context

Phase 0's remaining unknown was whether we could drive portal ScreenCast ourselves and receive
zero-copy dmabuf frames with explicit DRM format-modifier negotiation — the thing the plan called
the real go/no-go.

## Result: it works

`tools/capture-spike --stage 2` on the development box (Hyprland 0.56.2, portal 1.4.1,
PipeWire 1.6.8, NVIDIA 610.57.04):

```
portal ScreenCast version 6
AvailableSourceTypes 0x7 (monitor|window|virtual), AvailableCursorModes 0x3 (hidden|embedded)
node 377  2560x1440 @ (0,0)  type=monitor  mapping_id=DP-3
negotiated 2560x1440 BGRA (drm AR24) modifier LINEAR fps 0/1
fixating modifier LINEAR for BGRA
buffer type: DmaBuf   planes: 1   [0] offset=0 stride=10240 maxsize=0
frames: 30 dmabuf, 0 shm
```

Stride 10240 = 2560 x 4, correct for BGRA. **Single plane, LINEAR modifier** — the best possible
case for ADR-0007's cross-GPU transfer, since a linear single-plane buffer is the layout most likely
to import on a second vendor's GPU.

## What the protocol actually requires

Four things, each of which produced `no more input formats` when missing or wrong. Recorded because
none is obvious and all cost real time:

1. **Predict the Request object path and subscribe before calling.** Every portal method returns a
   Request path and answers later via a `Response` signal. The path is derived from our unique bus
   name (leading `:` stripped, `.` -> `_`) plus a token we choose, so we must subscribe *before*
   issuing the call or the reply can outrun the subscription.
2. **Modifier negotiation is a two-round dance.** Advertise the modifier list as a
   `CHOICE_Enum` with `SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE`; the server
   replies with the *intersection*, still unfixated; we must then re-advertise **exactly one**
   modifier via `pw_stream_update_params` and wait for the next `param_changed`. Omitting this
   fixation step fails negotiation outright.
3. **`SPA_POD_PROP_FLAG_DONT_FIXATE` must be honoured by checking the prop flags** on the returned
   format, not assumed. `spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier)` then test
   `flags & SPA_POD_PROP_FLAG_DONT_FIXATE`.
4. **Buffer data types must be declared after format agreement.** `SPA_PARAM_Buffers` with
   `SPA_PARAM_BUFFERS_dataType` as a flags-choice including `1u << SPA_DATA_DmaBuf`. Without
   DmaBuf in the mask, frames arrive as shm even when the modifier was negotiated.

Two C++-specific traps: SPA's `SPA_RECTANGLE`/`SPA_FRACTION` are C compound literals whose address
cannot be taken in C++ (use named constants), and `std::format` needs a compile-time format string
so runtime-built shell commands must be concatenated.

## The failure that wasted the most time: a stale restore token

`--restore-token <tok>` with a token from an earlier session caused every negotiation to fail with
`no more input formats`, with `param_changed` never firing. The identical code on a **fresh**
session, picker and all, negotiated immediately. The portal happily returns a session handle and a
node id for a token it cannot really restore, and the resulting node never produces a usable format.

**Consequences for the real implementation:**

- **Never trust a restore token blindly.** Treat a negotiation failure on a restored session as
  "token invalid": discard it and fall back to a fresh session with the picker, rather than
  reporting a capture error to the user.
- Persist restore tokens, but **validate by successful negotiation**, not by the portal's acceptance
  of the token.
- Diagnostics must distinguish "the compositor offered nothing we can use" from "this session is
  dead". `--dump-formats` exists for exactly this, and asks the node for its `EnumFormat` params
  via the registry rather than guessing.

## What the compositor actually offers

From `capture-spike --dump-formats` on a monitor node (development box, NVIDIA primary):

```
EnumFormat[0]  video/raw  format BGRA
  modifier { 12 x NVIDIA block-linear (0x03...), DRM_FORMAT_MOD_INVALID }
  size 2560x1440   framerate 0/1   maxFramerate [1/1, 120/1]
EnumFormat[1]  video/raw  format { BGRA, BGRx }      <- shm fallback, no modifier prop
  size 2560x1440   framerate 0/1   maxFramerate [1/1, 120/1]
```

Note `framerate` is **fixed at 0/1** ("source-driven") with the real bound in `maxFramerate`, and
that the modifier list is per-format. `DRM_FORMAT_MOD_LINEAR` is *not* in the advertised list on
this node, yet LINEAR is what the fixation round settled on — so the advertised enumeration is not
the whole truth, and the fixation result is the only authority on what we actually got.

## Still open

- **Stage 3: Vulkan import.** Bring the dmabuf fd into a `VkImage` via
  `VK_EXT_image_drm_format_modifier` + `VK_KHR_external_memory_fd`, with `sync_file` acquire/release
  on an exported timeline semaphore, and prove correctness by readback.
- The modifier list we advertise should come from
  `vkGetPhysicalDeviceFormatProperties2` + `VkDrmFormatModifierPropertiesListEXT`, not a guess.
- Cross-vendor import (NVIDIA-exported linear dmabuf -> Intel) for the VAAPI encode path.
- Re-run all of the above on `argus` (Turing + Gen9.5), per ADR-0008.
