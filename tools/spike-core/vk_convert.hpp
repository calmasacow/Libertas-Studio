// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "vk_import.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace libertas::spike {

/// How to interpret and encode colour. ADR-0014: these are *configuration*, never detection -
/// the capture device will happily claim a range it does not deliver, so the only honest source
/// of truth is what the user tells us.
struct ColorCfg {
    bool full_range = false;  ///< limited (16-235) by default, which is what broadcast expects
    bool bt601 = false;       ///< BT.709 by default; 601 is for SD-era sources
};

/// The host-side description of one converted frame. `data` is tightly packed NV12:
/// `height` rows of `y_stride`, then `height/2` rows of `uv_stride` starting at `uv_offset`.
struct Nv12Frame {
    std::vector<std::uint8_t> data;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t y_stride = 0;
    std::uint32_t uv_stride = 0;
    std::uint32_t uv_offset = 0;
};

/// Converts an imported/exported BGRA image to NV12 on the GPU and reads back the result.
///
/// This replaces "read back BGRA, then sws_scale on the CPU". It moves 1.5 bytes/pixel instead of
/// 4 and spends no CPU on the conversion at all (ADR-0019).
///
/// `written_by_foreign` must be true when a compositor produced the contents, so the barrier
/// acquires queue-family ownership - the same rule as the readback path.
bool vk_convert_nv12(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                     const ColorCfg& color, Nv12Frame& out, std::string& error);

/// A short description of the convert pipeline's state, for logging.
std::string vk_convert_note(VkCtx& ctx);

} // namespace libertas::spike
