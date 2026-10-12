// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "stream.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libertas::spike {

struct VkCtx;

void vk_destroy(VkCtx* ctx);

/// Deleter declared here so unique_ptr works with the incomplete VkCtx type.
struct VkCtxDeleter {
    void operator()(VkCtx* p) const { vk_destroy(p); }
};
using VkCtxPtr = std::unique_ptr<VkCtx, VkCtxDeleter>;

/// Creates an instance + device with the external-memory/dmabuf extensions.
/// `prefer_drm_node` selects the device whose DRM primary/render node matches, when non-empty;
/// otherwise the first discrete GPU wins. Device choice is never by enumeration index (ADR-0008).
VkCtxPtr vk_init(const std::string& prefer_drm_node, std::string& error);

/// Reports what the device actually supports, so PipeWire negotiation can advertise the truth
/// rather than a guess. Returns modifiers valid for sampling/transfer of `drm_format`.
std::vector<std::uint64_t> vk_supported_modifiers(VkCtx& ctx, std::uint32_t drm_format);

/// A short description of the selected device, for logging.
std::string vk_device_name(VkCtx& ctx);

/// Imports one dmabuf frame as a VkImage and copies it back to host memory, proving the import
/// produced real pixels. `out` receives width*height*4 bytes in the image's byte order.
bool vk_import_and_readback(VkCtx& ctx, const CapturedFrame& frame, std::vector<std::uint8_t>& out,
                            std::string& error);

} // namespace libertas::spike
