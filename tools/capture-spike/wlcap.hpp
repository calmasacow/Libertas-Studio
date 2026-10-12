// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vk_import.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libertas::spike {

/// One output, as the compositor names it. `name` is the connector ("DP-1"), which is the same
/// identifier Hyprland and the portal's `mapping_id` use, so sources are comparable across
/// backends and stable across restarts - unlike a portal picker selection.
struct WlOutputInfo {
    std::string name;
    std::string description;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t refresh_mhz = 0;
};

/// A DRM format plus the modifiers the compositor will accept for it.
struct WlCapFormat {
    std::uint32_t drm_format = 0;
    std::vector<std::uint64_t> modifiers;
};

/// What the compositor says it can deliver for a capture session. Note this comes from the
/// compositor itself, not from an intermediary - which is the whole reason for this backend.
struct WlCapCaps {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t dmabuf_device = 0; ///< dev_t of the device the buffers must be allocated on
    std::vector<WlCapFormat> formats;
    std::vector<std::uint32_t> shm_formats;
};

struct WlCap;
void wlcap_close(WlCap*);
struct WlCapDeleter {
    void operator()(WlCap* p) const { wlcap_close(p); }
};
using WlCapPtr = std::unique_ptr<WlCap, WlCapDeleter>;

/// Connects to the compositor and binds the capture globals.
/// Fails with a specific message when a required global is absent, so an unsupported compositor is
/// distinguishable from a broken one.
WlCapPtr wlcap_open(std::string& error);

const std::vector<WlOutputInfo>& wlcap_outputs(WlCap& cap);

/// Creates a capture session for `output_name` (empty = the first output) and waits for the
/// compositor to describe it. No picker, no dialog, no user interaction.
bool wlcap_begin_session(WlCap& cap, const std::string& output_name, bool with_cursor,
                         WlCapCaps& out, std::string& error);

/// Wraps an exported Vulkan image as a wl_buffer and captures exactly one frame into it.
/// On success the VkImage holds the screen contents. Blocks until `ready` or `failed`.
///
/// `slot` lets a caller rotate between several images. Capturing repeatedly into ONE image
/// serializes the compositor against us - it cannot draw the next frame while we hold its only
/// destination - so a single-buffer measurement understates the achievable rate and invents a
/// latency tail that the protocol does not have.
bool wlcap_capture_frame(WlCap& cap, const VkExportedImage& img, std::uint32_t drm_format,
                         std::string& error);

/// Registers `img` as a reusable buffer slot. The wl_buffer is created once and kept, rather than
/// built and destroyed per frame, which is what the real capture worker must do.
bool wlcap_add_slot(WlCap& cap, const VkExportedImage& img, std::uint32_t drm_format,
                    std::string& error);

/// Captures one frame into slot `index`, blocking until that frame is ready.
///
/// `out_presented_ns` receives the compositor's presentation timestamp for the frame, when it
/// sends one. That is the only way to tell "the capture path is slow" from "the screen did not
/// change": inter-arrival time measures the latter, and on an idle desktop it measures nothing
/// else at all.
bool wlcap_capture_slot(WlCap& cap, std::size_t index, std::uint32_t width, std::uint32_t height,
                        std::uint64_t* out_presented_ns, std::string& error);

/// major:minor of the dmabuf device, for matching against a Vulkan device's DRM properties.
std::string wlcap_device_str(std::uint64_t dev);

} // namespace libertas::spike
