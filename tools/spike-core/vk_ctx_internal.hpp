// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Vulkan context, shared between the spike-core translation units that need its guts.
//
// This is an *internal* header: it is not in vk_import.hpp because callers outside spike-core
// are deliberately given only an opaque VkCtx. Everything here is implementation detail that
// vk_import.cpp and vk_convert.cpp both have to agree on.
#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

namespace libertas::spike {

/// Owned by vk_convert.cpp; opaque here so this header stays free of its internals.
struct ConvertState;
struct VkCtx;

/// Tears down the convert pipeline. Defined in vk_convert.cpp, called from vk_destroy.
void vk_convert_destroy(VkCtx& ctx);

/// Staging resources for host readback, kept alive across frames. See `readback_image` for why
/// per-frame allocation and an uncached memory type were both wrong.
struct ReadbackStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize capacity = 0;
    std::uint32_t type_index = UINT32_MAX;
    bool cached = false;
    bool coherent = false;
};

struct VkCtx {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::string device_name;
    bool have_foreign_queue = false;

    PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties = nullptr;
    PFN_vkGetImageDrmFormatModifierPropertiesEXT get_image_modifier_properties = nullptr;
    PFN_vkGetMemoryFdKHR get_memory_fd = nullptr;

    ReadbackStaging readback;

    /// Compute pipeline + buffers for BGRA->NV12, built on first use. Owned by vk_convert.cpp.
    ConvertState* convert = nullptr;
};

} // namespace libertas::spike
