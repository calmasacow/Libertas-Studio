<!--
SPDX-FileCopyrightText: Libertas-Studio contributors
SPDX-License-Identifier: GPL-3.0-or-later
-->
# Libertas-Studio

A live streaming and recording studio for Linux, built clean-sheet. Omarchy (Arch + Hyprland) is the
bedrock target; the rest of Linux follows, then macOS and Windows.

**Status: pre-alpha.** Nothing streams yet. What exists today is the build foundation, the
architectural decision record, and `libertas probe`.

## Why

Libertas is not an OBS fork and uses no `libobs` code. Two reasons:

- **Independence.** A new frontend over `libobs` would still mean tracking upstream releases,
  GPL-linking to their code and inheriting their engine decisions. That is a reskin, not
  independence.
- **Linux is structurally last in line in OBS.** There is no Vulkan backend — Windows gets a
  dedicated D3D11 backend and macOS a dedicated Metal one, while Linux rides the shared legacy
  OpenGL layer that also serves as everyone's fallback. Windows capture has ~2.3x the code Linux
  gets. Upstream CI builds exactly one Linux configuration (`ubuntu`), never Arch or Hyprland.

See [`docs/adr/0001-no-libobs.md`](docs/adr/0001-no-libobs.md) for the full reasoning and the
rejected alternatives.

## Design commitments

| | |
|---|---|
| **Wayland-native** | No X11 code at all. Global hotkeys via `org.freedesktop.portal.GlobalShortcuts`, capture via portal ScreenCast + PipeWire. ([ADR-0003](docs/adr/0003-wayland-native-no-x11.md)) |
| **Multi-process** | A tiny supervisor owns the document; the GPU, encoders and outputs live in one render process; risky work (media demux, browser engines, device SDKs) runs in pooled workers. The UI has zero authority and may be killed freely. ([ADR-0005](docs/adr/0005-multiprocess-isolation.md)) |
| **No in-process plugins** | Extensions are shader text, Wasm components, or separate worker processes — never `dlopen`'d into our address space. This is what keeps the engine changeable in year three. ([ADR-0004](docs/adr/0004-no-in-process-plugins.md)) |
| **Vulkan, explicit sync** | Composite in scene-linear RGBA16F. Encode on the same `VkDevice` via `VK_KHR_video_encode_queue` where available. ([ADR-0006](docs/adr/0006-nvenc-interop-path.md)) |
| **Multi-GPU encode** | Concurrent encode across different GPUs at different resolutions is a first-class requirement, not a bolt-on. ([ADR-0007](docs/adr/0007-dual-gpu-concurrent-encode.md)) |
| **Theme-aware** | Follows the live Omarchy theme; ships a token fallback so other platforms are theme-driven too. |

The honest trade-off is recorded too: clean sheet is the harder road, 1-2 years to real parity, and
the first six months will feel worse than OBS.

## Building

Requires a C++23 compiler, CMake 3.28+, and development headers for Vulkan, libdrm, PipeWire, GIO
and FFmpeg. On Arch:

```sh
sudo pacman -S --needed base-devel cmake ninja vulkan-headers vulkan-icd-loader \
                        libdrm pipewire glib2 ffmpeg
```

```sh
cmake --preset linux
cmake --build --preset linux
./build/linux/apps/libertas/libertas probe
```

Presets: `linux`, `linux-debug`, `linux-ci` (warnings are errors), `linux-asan`, `linux-tsan`.
Without `ninja`, configure manually with `-G "Unix Makefiles"`.

## `libertas probe`

Reports everything about a machine that affects capture, composite and encode: GPUs and their DRM
nodes, Vulkan extension support per device (including video encode), the libva driver actually bound
to each render node, available encoders, portal interfaces, PipeWire, and the current Omarchy theme.

It exists because capability varies more between machines than it has any right to — two machines on
the same distro and driver version disagreed on QuickSync availability, Vulkan device ordering and
AV1 support. **Nothing about device capability is decided at compile time.** Attach its output to any
bug report.

It exits non-zero if it finds itself running under XWayland, which is refused deliberately rather
than silently degrading.

## Licence

GPL-3.0-or-later. See [`docs/adr/0002-gpl3.md`](docs/adr/0002-gpl3.md) for why, including the
rejected permissive options.
