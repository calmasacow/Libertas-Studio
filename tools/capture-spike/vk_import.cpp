// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vulkan dmabuf import for captured frames.
//
// The interesting parts, all of which are easy to get subtly wrong:
//   * the image must be created with VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and an *explicit*
//     modifier + per-plane layout taken from what PipeWire handed us;
//   * vkGetMemoryFdPropertiesKHR, not the image's memoryTypeBits alone, decides which memory types
//     may back an imported fd;
//   * VkImportMemoryFdInfoKHR takes ownership of the fd, so PipeWire's borrowed fd must be dup'd;
//   * the first barrier is a queue-family *acquire* from VK_QUEUE_FAMILY_FOREIGN_EXT, which is what
//     preserves the external writer's contents.

#include "vk_import.hpp"

#include <lcore/log.hpp>

#include <algorithm>
#include <array>
#include <cstring>

#include <unistd.h>
#include <vulkan/vulkan.h>

#include <drm_fourcc.h>

namespace libertas::spike {

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
};

namespace {

/// DRM fourcc -> Vulkan format. DRM names channels in little-endian word order, Vulkan in byte
/// order, so DRM_FORMAT_ARGB8888 ("AR24") is B8G8R8A8 in Vulkan terms.
VkFormat drm_to_vk(std::uint32_t drm) {
    switch (drm) {
    case DRM_FORMAT_ARGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_XRGB8888: return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    case DRM_FORMAT_XBGR8888: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool device_has_ext(VkPhysicalDevice d, const char* name) {
    std::uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(d, nullptr, &n, exts.data());
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

} // namespace

VkCtxPtr vk_init(const std::string& prefer_drm_node, std::string& error) {
    VkCtxPtr ctx(new VkCtx());

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "libertas-capture-spike";
    app.apiVersion = VK_API_VERSION_1_3;

    static constexpr std::array<const char*, 2> kInstExts{
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<std::uint32_t>(kInstExts.size());
    ici.ppEnabledExtensionNames = kInstExts.data();

    if (VkResult r = vkCreateInstance(&ici, nullptr, &ctx->instance); r != VK_SUCCESS) {
        error = std::format("vkCreateInstance failed ({})", static_cast<int>(r));
        return nullptr;
    }

    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(ctx->instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(ctx->instance, &count, devices.data());
    if (count == 0) {
        error = "no Vulkan devices";
        return nullptr;
    }

    // Required for importing dmabufs with an explicit modifier.
    static constexpr std::array<const char*, 5> kNeeded{
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
    };

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices) {
        bool ok = true;
        for (const char* e : kNeeded)
            if (std::strcmp(e, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) != 0 &&
                !device_has_ext(d, e))
                ok = false;
        if (!ok)
            continue;

        VkPhysicalDeviceDrmPropertiesEXT drm{};
        drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drm;
        vkGetPhysicalDeviceProperties2(d, &p2);

        // Match by DRM node when asked, never by enumeration index (ADR-0008).
        if (!prefer_drm_node.empty()) {
            const std::string want = std::format("{}:{}", drm.renderMajor, drm.renderMinor);
            const std::string want_primary = std::format("{}:{}", drm.primaryMajor,
                                                          drm.primaryMinor);
            if (prefer_drm_node != want && prefer_drm_node != want_primary)
                continue;
            chosen = d;
            break;
        }
        if (chosen == VK_NULL_HANDLE ||
            p2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            chosen = d;
    }

    if (chosen == VK_NULL_HANDLE) {
        error = "no device with the required external-memory/dmabuf extensions";
        return nullptr;
    }
    ctx->phys = chosen;
    ctx->have_foreign_queue =
        device_has_ext(chosen, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(chosen, &props);
    ctx->device_name = props.deviceName;

    std::uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, qfp.data());
    bool found_q = false;
    for (std::uint32_t i = 0; i < qn; ++i) {
        if ((qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            ctx->queue_family = i;
            found_q = true;
            break;
        }
    }
    if (!found_q) {
        error = "no graphics queue family";
        return nullptr;
    }

    std::vector<const char*> dev_exts{
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    };
    if (ctx->have_foreign_queue)
        dev_exts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = ctx->queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();

    if (VkResult r = vkCreateDevice(chosen, &dci, nullptr, &ctx->device); r != VK_SUCCESS) {
        error = std::format("vkCreateDevice failed ({})", static_cast<int>(r));
        return nullptr;
    }
    vkGetDeviceQueue(ctx->device, ctx->queue_family, 0, &ctx->queue);

    ctx->get_memory_fd_properties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdPropertiesKHR"));
    ctx->get_image_modifier_properties =
        reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(
            vkGetDeviceProcAddr(ctx->device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    ctx->get_memory_fd =
        reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(ctx->device, "vkGetMemoryFdKHR"));
    if (ctx->get_memory_fd_properties == nullptr) {
        error = "vkGetMemoryFdPropertiesKHR unavailable";
        return nullptr;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx->queue_family;
    if (vkCreateCommandPool(ctx->device, &pci, nullptr, &ctx->pool) != VK_SUCCESS) {
        error = "vkCreateCommandPool failed";
        return nullptr;
    }
    return ctx;
}

void vk_destroy(VkCtx* ctx) {
    // Also frees the struct: this is the unique_ptr deleter target.
    if (ctx == nullptr)
        return;
    if (ctx->pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(ctx->device, ctx->pool, nullptr);
    if (ctx->device != VK_NULL_HANDLE)
        vkDestroyDevice(ctx->device, nullptr);
    if (ctx->instance != VK_NULL_HANDLE)
        vkDestroyInstance(ctx->instance, nullptr);
    delete ctx;
}

std::string vk_device_name(VkCtx& ctx) { return ctx.device_name; }

std::vector<std::uint64_t> vk_supported_modifiers(VkCtx& ctx, std::uint32_t drm_format) {
    std::vector<std::uint64_t> out;
    const VkFormat vkf = drm_to_vk(drm_format);
    if (vkf == VK_FORMAT_UNDEFINED)
        return out;

    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp.pNext = &list;

    vkGetPhysicalDeviceFormatProperties2(ctx.phys, vkf, &fp);
    if (list.drmFormatModifierCount == 0)
        return out;

    std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    vkGetPhysicalDeviceFormatProperties2(ctx.phys, vkf, &fp);

    for (const auto& p : props) {
        // We need to sample from, or copy out of, the imported image.
        constexpr VkFormatFeatureFlags kWant =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((p.drmFormatModifierTilingFeatures & kWant) == kWant)
            out.push_back(p.drmFormatModifier);
    }
    return out;
}

namespace {

/// Copies an image to host memory as tightly-packed BGRA. Shared by the import and export paths so
/// the two cannot drift: a readback that differs between them would make their results
/// incomparable, which is the whole point of having both.
///
/// `written_by_foreign` selects a queue-family ownership *acquire* as the first barrier. That
/// acquire is what makes an external writer's contents visible; omitting it is undefined behaviour
/// that happens to work on some drivers, which is the worst kind of bug to ship.
bool readback_image(VkCtx& ctx, VkImage image, std::uint32_t width, std::uint32_t height,
                    bool written_by_foreign, std::vector<std::uint8_t>& out, std::string& error) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer staging = VK_NULL_HANDLE;
    if (vkCreateBuffer(ctx.device, &bci, nullptr, &staging) != VK_SUCCESS) {
        error = "vkCreateBuffer (staging) failed";
        return false;
    }

    VkMemoryRequirements bmr{};
    vkGetBufferMemoryRequirements(ctx.device, staging, &bmr);

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(ctx.phys, &mp);
    std::uint32_t host_type = UINT32_MAX;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        const bool fits = (bmr.memoryTypeBits & (1u << i)) != 0u;
        const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (fits && (mp.memoryTypes[i].propertyFlags & want) == want) {
            host_type = i;
            break;
        }
    }
    if (host_type == UINT32_MAX) {
        error = "no host-visible coherent memory type";
        vkDestroyBuffer(ctx.device, staging, nullptr);
        return false;
    }

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = bmr.size;
    bai.memoryTypeIndex = host_type;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    vkAllocateMemory(ctx.device, &bai, nullptr, &staging_mem);
    vkBindBufferMemory(ctx.device, staging, staging_mem, 0);

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = ctx.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.device, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier acquire{};
    acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acquire.srcAccessMask = 0;
    acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (written_by_foreign) {
        acquire.srcQueueFamilyIndex =
            ctx.have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = ctx.queue_family;
    } else {
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        acquire.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    acquire.image = image;
    acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &acquire);

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.bufferRowLength = width;
    copy.bufferImageHeight = height;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(ctx.device, &fci, nullptr, &fence);

    bool ok = true;
    if (vkQueueSubmit(ctx.queue, 1, &si, fence) != VK_SUCCESS) {
        error = "vkQueueSubmit failed";
        ok = false;
    } else if (vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, 2'000'000'000ULL) != VK_SUCCESS) {
        error = "fence wait timed out";
        ok = false;
    } else {
        void* mapped = nullptr;
        if (vkMapMemory(ctx.device, staging_mem, 0, bytes, 0, &mapped) == VK_SUCCESS) {
            out.resize(bytes);
            std::memcpy(out.data(), mapped, bytes);
            vkUnmapMemory(ctx.device, staging_mem);
        } else {
            error = "vkMapMemory failed";
            ok = false;
        }
    }

    vkDestroyFence(ctx.device, fence, nullptr);
    vkFreeCommandBuffers(ctx.device, ctx.pool, 1, &cmd);
    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, staging_mem, nullptr);
    return ok;
}

} // namespace

bool vk_import_and_readback(VkCtx& ctx, const CapturedFrame& frame, std::vector<std::uint8_t>& out,
                            std::string& error) {
    if (!frame.is_dmabuf || frame.planes.empty() || frame.planes.front().fd < 0) {
        error = "frame is not a dmabuf";
        return false;
    }
    const VkFormat vkf = drm_to_vk(frame.drm_format);
    if (vkf == VK_FORMAT_UNDEFINED) {
        error = std::format("unmapped DRM format {}", fourcc_str(frame.drm_format));
        return false;
    }

    // ---- create the image with an explicit modifier + the plane layouts PipeWire gave us
    std::vector<VkSubresourceLayout> layouts(frame.planes.size());
    for (std::size_t i = 0; i < frame.planes.size(); ++i) {
        layouts[i] = {};
        layouts[i].offset = frame.planes[i].offset;
        layouts[i].rowPitch = frame.planes[i].stride;
        // size/arrayPitch/depthPitch must be 0 here; the implementation derives them.
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info{};
    mod_info.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
    mod_info.drmFormatModifier = frame.modifier;
    mod_info.drmFormatModifierPlaneCount = static_cast<std::uint32_t>(layouts.size());
    mod_info.pPlaneLayouts = layouts.data();

    VkExternalMemoryImageCreateInfo ext_img{};
    ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    ext_img.pNext = &mod_info;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext_img;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkf;
    ici.extent = {frame.width, frame.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(ctx.device, &ici, nullptr, &image); r != VK_SUCCESS) {
        error = std::format("vkCreateImage (modifier 0x{:x}) failed ({})", frame.modifier,
                            static_cast<int>(r));
        return false;
    }

    auto cleanup_image = [&] { vkDestroyImage(ctx.device, image, nullptr); };

    // ---- which memory types may back this fd?
    VkMemoryFdPropertiesKHR fd_props{};
    fd_props.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (ctx.get_memory_fd_properties(ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                     frame.planes.front().fd, &fd_props) != VK_SUCCESS) {
        error = "vkGetMemoryFdPropertiesKHR failed";
        cleanup_image();
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device, image, &mr);
    const std::uint32_t usable = mr.memoryTypeBits & fd_props.memoryTypeBits;
    if (usable == 0) {
        error = "no memory type satisfies both the image and the imported fd";
        cleanup_image();
        return false;
    }
    std::uint32_t mem_type = 0;
    for (std::uint32_t i = 0; i < 32; ++i)
        if ((usable & (1u << i)) != 0u) {
            mem_type = i;
            break;
        }

    // VkImportMemoryFdInfoKHR takes ownership of the fd, and PipeWire's is borrowed.
    const int dup_fd = ::dup(frame.planes.front().fd);
    if (dup_fd < 0) {
        error = std::format("dup(dmabuf fd) failed: {}", std::strerror(errno));
        cleanup_image();
        return false;
    }

    VkImportMemoryFdInfoKHR import_info{};
    import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import_info.fd = dup_fd;

    VkMemoryDedicatedAllocateInfo dedicated{};
    dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.image = image;
    dedicated.pNext = &import_info;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateMemory(ctx.device, &mai, nullptr, &memory); r != VK_SUCCESS) {
        error = std::format("vkAllocateMemory (import) failed ({})", static_cast<int>(r));
        ::close(dup_fd); // not consumed on failure
        cleanup_image();
        return false;
    }
    if (vkBindImageMemory(ctx.device, image, memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        cleanup_image();
        return false;
    }

    const bool ok = readback_image(ctx, image, frame.width, frame.height, true, out, error);
    vkFreeMemory(ctx.device, memory, nullptr);
    cleanup_image();
    return ok;
}


// ---------------------------------------------------------------- export: we allocate, they draw

namespace {

/// Picks a DEVICE_LOCAL memory type from a requirements mask.
std::uint32_t device_local_memory_type(VkPhysicalDevice phys, std::uint32_t type_bits) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) != 0u &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u)
            return i;
    return UINT32_MAX;
}

/// How many memory planes a given modifier uses for this format. Guessing 1 works for linear
/// single-plane RGBA and then silently corrupts anything else, so ask the driver.
std::uint32_t modifier_plane_count(VkPhysicalDevice phys, VkFormat fmt, std::uint64_t modifier) {
    VkDrmFormatModifierPropertiesListEXT list{};
    list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    VkFormatProperties2 fp{};
    fp.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    fp.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
    std::vector<VkDrmFormatModifierPropertiesEXT> props(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = props.data();
    vkGetPhysicalDeviceFormatProperties2(phys, fmt, &fp);
    for (const auto& p : props)
        if (p.drmFormatModifier == modifier)
            return p.drmFormatModifierPlaneCount;
    return 0;
}

} // namespace

bool vk_create_exportable(VkCtx& ctx, std::uint32_t width, std::uint32_t height,
                          std::uint32_t drm_format, const std::vector<std::uint64_t>& allowed,
                          VkExportedImage& out, std::string& error) {
    const VkFormat fmt = drm_to_vk(drm_format);
    if (fmt == VK_FORMAT_UNDEFINED) {
        error = std::format("unsupported drm format 0x{:08x}", drm_format);
        return false;
    }
    if (allowed.empty()) {
        error = "no candidate modifiers";
        return false;
    }
    if (ctx.get_memory_fd == nullptr || ctx.get_image_modifier_properties == nullptr) {
        error = "vkGetMemoryFdKHR / vkGetImageDrmFormatModifierPropertiesEXT unavailable";
        return false;
    }

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // Hand the driver the whole candidate list and let it choose; we read back which one it took.
    // Passing a single modifier instead would turn a perfectly good second choice into a failure.
    VkImageDrmFormatModifierListCreateInfoEXT modlist{};
    modlist.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT;
    modlist.drmFormatModifierCount = static_cast<std::uint32_t>(allowed.size());
    modlist.pDrmFormatModifiers = allowed.data();
    modlist.pNext = &ext;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &modlist;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    // TRANSFER_DST and COLOR_ATTACHMENT matter: the compositor blits or renders into this image,
    // and a usage set that omits them can make the driver pick a layout it cannot write to.
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (VkResult r = vkCreateImage(ctx.device, &ici, nullptr, &image); r != VK_SUCCESS) {
        error = std::format("vkCreateImage (exportable) failed ({})", static_cast<int>(r));
        return false;
    }

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(ctx.device, image, &mr);
    const std::uint32_t type = device_local_memory_type(ctx.phys, mr.memoryTypeBits);
    if (type == UINT32_MAX) {
        error = "no device-local memory type for the exportable image";
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    VkExportMemoryAllocateInfo exp{};
    exp.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    // A dedicated allocation is required for an exported image on most drivers, and is what makes
    // the exported fd describe exactly this image rather than a suballocation of a heap.
    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = image;
    ded.pNext = &exp;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &ded;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = type;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (VkResult r = vkAllocateMemory(ctx.device, &mai, nullptr, &memory); r != VK_SUCCESS) {
        error = std::format("vkAllocateMemory (exportable) failed ({})", static_cast<int>(r));
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }
    if (vkBindImageMemory(ctx.device, image, memory, 0) != VK_SUCCESS) {
        error = "vkBindImageMemory (exportable) failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    VkImageDrmFormatModifierPropertiesEXT chosen{};
    chosen.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    if (ctx.get_image_modifier_properties(ctx.device, image, &chosen) != VK_SUCCESS) {
        error = "vkGetImageDrmFormatModifierPropertiesEXT failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    std::uint32_t planes = modifier_plane_count(ctx.phys, fmt, chosen.drmFormatModifier);
    if (planes == 0 || planes > 4)
        planes = 1;

    static constexpr VkImageAspectFlagBits kPlaneAspect[4]{
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT, VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT};
    for (std::uint32_t i = 0; i < planes; ++i) {
        VkImageSubresource sub{};
        sub.aspectMask = kPlaneAspect[i];
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(ctx.device, image, &sub, &layout);
        out.offsets[i] = static_cast<std::uint32_t>(layout.offset);
        out.strides[i] = static_cast<std::uint32_t>(layout.rowPitch);
    }

    VkMemoryGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    gfi.memory = memory;
    gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    if (ctx.get_memory_fd(ctx.device, &gfi, &fd) != VK_SUCCESS || fd < 0) {
        error = "vkGetMemoryFdKHR failed";
        vkFreeMemory(ctx.device, memory, nullptr);
        vkDestroyImage(ctx.device, image, nullptr);
        return false;
    }

    out.image = image;
    out.memory = memory;
    out.dmabuf_fd = fd;
    out.width = width;
    out.height = height;
    out.modifier = chosen.drmFormatModifier;
    out.n_planes = planes;
    return true;
}

void vk_destroy_exportable(VkCtx& ctx, VkExportedImage& img) {
    if (img.dmabuf_fd >= 0) {
        ::close(img.dmabuf_fd);
        img.dmabuf_fd = -1;
    }
    if (img.image != nullptr) {
        vkDestroyImage(ctx.device, static_cast<VkImage>(img.image), nullptr);
        img.image = nullptr;
    }
    if (img.memory != nullptr) {
        vkFreeMemory(ctx.device, static_cast<VkDeviceMemory>(img.memory), nullptr);
        img.memory = nullptr;
    }
}

bool vk_readback_exported(VkCtx& ctx, const VkExportedImage& img, bool written_by_foreign,
                          std::vector<std::uint8_t>& out, std::string& error) {
    return readback_image(ctx, static_cast<VkImage>(img.image), img.width, img.height,
                          written_by_foreign, out, error);
}

} // namespace libertas::spike
