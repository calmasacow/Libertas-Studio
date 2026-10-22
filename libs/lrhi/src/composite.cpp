// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shaders/composite_spv.h"
#include "shaders/filter_spv.h"
#include "shaders/preview_spv.h"
#include "vk_ctx_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <drm_fourcc.h>
#include <format>
#include <lcomp/filters.hpp>
#include <lrhi/composite.hpp>
#include <map>
#include <poll.h>
#include <stdexcept>

namespace libertas::rhi {
namespace {
void checked(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::format("{} ({})", action, static_cast<int>(result)));
}

std::uint32_t memory_type(VkCtx& c, std::uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &props);
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    throw std::runtime_error("no suitable GPU memory type");
}

struct Image {
    VkCtx& c;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int width = 0, height = 0;
    bool owns = true;

    ~Image() {
        if (view)
            vkDestroyImageView(c.device, view, nullptr);
        if (owns && image)
            vkDestroyImage(c.device, image, nullptr);
        if (memory)
            vkFreeMemory(c.device, memory, nullptr);
    }
};

struct Buffer {
    VkCtx& c;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;

    ~Buffer() {
        if (mapped)
            vkUnmapMemory(c.device, memory);
        if (buffer)
            vkDestroyBuffer(c.device, buffer, nullptr);
        if (memory)
            vkFreeMemory(c.device, memory, nullptr);
    }
};

struct Push {
    std::array<int, 4> rect{};
    std::array<int, 4> flags{};
    std::array<float, 4> colour{};
    std::array<float, 4> opacity{};
    std::array<int, 4> crop{};
};

static_assert(sizeof(Push) == 80);
// Keep composite.comp's mode constants aligned with the shared plan.
static_assert(static_cast<int>(ldoc::BlendMode::Normal) == 0 &&
              static_cast<int>(ldoc::BlendMode::Darken) == 2 &&
              static_cast<int>(ldoc::BlendMode::Multiply) == 3 &&
              static_cast<int>(ldoc::BlendMode::Lighten) == 6 &&
              static_cast<int>(ldoc::BlendMode::Screen) == 7 &&
              static_cast<int>(ldoc::BlendMode::LinearDodge) == 9 &&
              static_cast<int>(ldoc::BlendMode::Subtract) == 19);
} // namespace

struct GpuCompositor::Impl {
    VkCtx* c = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline composite = VK_NULL_HANDLE, preview = VK_NULL_HANDLE, filter = VK_NULL_HANDLE;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Image>> images;
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::map<VkImage, std::unique_ptr<Image>> foreign;
    std::vector<Image*> acquired;
    std::unique_ptr<Buffer> readback;
    std::size_t image_at = 0, upload_at = 0;
    std::uint64_t allocated = 0;

    struct Auxiliary {
        std::shared_ptr<lcomp::Picture> pixels;
        std::unique_ptr<Image> image;
    };

    std::vector<Auxiliary> auxiliaries;
    std::uint64_t auxiliary_bytes = 0;

    struct History {
        int width = 0, height = 0;
        float delay = 0;
        std::uint64_t last = 0;
        std::deque<std::pair<std::uint64_t, std::unique_ptr<Image>>> frames;
    };

    std::map<ldoc::Id, History> histories;
    std::vector<std::unique_ptr<Image>> retired, recycled;
    std::vector<lcomp::Gap> runtime_gaps;

    struct MediaImage {
        ldoc::Id source;
        std::vector<ldoc::Effect> filters;
        std::shared_ptr<const void> frame;
        std::unique_ptr<Image> image;
    };

    std::vector<MediaImage> media_images;
    std::uint64_t media_bytes = 0;

    Image& delayed(Image& input, const lcomp::VideoFilter& op) {
        auto& h = histories[op.id];
        if (h.width != input.width || h.height != input.height || h.delay != op.values[0] ||
            op.time_ns < h.last) {
            for (auto& frame : h.frames)
                retired.push_back(std::move(frame.second));
            h = {input.width, input.height, op.values[0], 0, {}};
        }
        const auto delay = static_cast<std::uint64_t>(op.values[0] * 1000000);
        const auto due = op.time_ns > delay ? op.time_ns - delay : 0;
        while (h.frames.size() > 1 && h.frames[1].first <= due) {
            retired.push_back(std::move(h.frames.front().second));
            h.frames.pop_front();
        }
        std::uint64_t bytes = 0;
        for (const auto& [id, history] : histories)
            bytes += static_cast<std::uint64_t>(history.width) *
                     static_cast<std::uint64_t>(history.height) * 8 * history.frames.size();
        bool full = false;
        if (h.last != op.time_ns || h.frames.empty()) {
            const auto need = static_cast<std::uint64_t>(input.width) *
                              static_cast<std::uint64_t>(input.height) * 8;
            if (bytes + need > 256ULL * 1024 * 1024 || h.frames.size() >= 1200)
                full = true;
            else {
                std::unique_ptr<Image> saved;
                const auto reuse = std::ranges::find_if(recycled, [&](const auto& image) {
                    return image && image->width == input.width && image->height == input.height;
                });
                if (reuse != recycled.end()) {
                    saved = std::move(*reuse);
                    recycled.erase(reuse);
                } else {
                    target(input.width, input.height, VK_FORMAT_R16G16B16A16_SFLOAT);
                    saved = std::move(images[image_at - 1]);
                }
                barrier(*saved, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = copy.srcSubresource;
                copy.extent = {static_cast<unsigned>(input.width),
                               static_cast<unsigned>(input.height), 1};
                vkCmdCopyImage(cmd, input.image, VK_IMAGE_LAYOUT_GENERAL, saved->image,
                               VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
                barrier(*saved, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                h.frames.emplace_back(op.time_ns, std::move(saved));
            }
            h.last = op.time_ns;
        }
        if (!full && !h.frames.empty() && h.frames.front().first <= due)
            return *h.frames.front().second;
        auto& empty = target(input.width, input.height, VK_FORMAT_R16G16B16A16_SFLOAT);
        barrier(empty, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, empty.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        barrier(empty, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        runtime_gaps.push_back(
            {op.id, "Video delay",
             full ? lcomp::GapReason::UnsupportedEffect : lcomp::GapReason::NoFrame});
        return empty;
    }

    ~Impl() {
        if (!c)
            return;
        foreign.clear();
        auxiliaries.clear();
        media_images.clear();
        images.clear();
        uploads.clear();
        readback.reset();
        if (sampler)
            vkDestroySampler(c->device, sampler, nullptr);
        if (descriptors)
            vkDestroyDescriptorPool(c->device, descriptors, nullptr);
        if (preview)
            vkDestroyPipeline(c->device, preview, nullptr);
        if (filter)
            vkDestroyPipeline(c->device, filter, nullptr);
        if (composite)
            vkDestroyPipeline(c->device, composite, nullptr);
        if (layout)
            vkDestroyPipelineLayout(c->device, layout, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(c->device, set_layout, nullptr);
        if (fence)
            vkDestroyFence(c->device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(c->device, pool, nullptr);
    }

    VkPipeline pipeline(const std::uint32_t* code, std::size_t size) {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = size;
        info.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        checked(vkCreateShaderModule(c->device, &info, nullptr, &module), "create shader");
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline result = VK_NULL_HANDLE;
        const auto status =
            vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, nullptr, &result);
        vkDestroyShaderModule(c->device, module, nullptr);
        checked(status, "create composite pipeline");
        return result;
    }

    void init() {
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(c->phys, &features);
        if (!features.shaderInt64 || !features.shaderStorageImageExtendedFormats)
            throw std::runtime_error("device lacks composite shader features");
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(c->phys, VK_FORMAT_R16G16B16A16_SFLOAT, &props);
        constexpr auto required =
            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & required) != required)
            throw std::runtime_error("device lacks RGBA16F storage/sampling");
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = c->queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        checked(vkCreateCommandPool(c->device, &pi, nullptr, &pool), "create composite pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(c->device, &ai, &cmd),
                "allocate composite command buffer");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        checked(vkCreateFence(c->device, &fi, nullptr, &fence), "create composite fence");
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
             nullptr}};
        VkDescriptorSetLayoutCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        si.bindingCount = 3;
        si.pBindings = bindings;
        checked(vkCreateDescriptorSetLayout(c->device, &si, nullptr, &set_layout),
                "create descriptor layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1;
        li.pSetLayouts = &set_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(c->device, &li, nullptr, &layout), "create pipeline layout");
        composite = pipeline(kCompositeSpv, sizeof(kCompositeSpv));
        preview = pipeline(kPreviewSpv, sizeof(kPreviewSpv));
        filter = pipeline(kFilterSpv, sizeof(kFilterSpv));
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192}};
        VkDescriptorPoolCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        di.maxSets = 4096;
        di.poolSizeCount = 2;
        di.pPoolSizes = sizes;
        checked(vkCreateDescriptorPool(c->device, &di, nullptr, &descriptors),
                "create descriptor pool");
        VkSamplerCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sm.magFilter = sm.minFilter = VK_FILTER_NEAREST;
        sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        checked(vkCreateSampler(c->device, &sm, nullptr, &sampler), "create sampler");
    }

    void view(Image& image, bool opaque = false) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = image.format;
        vi.components.a = opaque ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        checked(vkCreateImageView(c->device, &vi, nullptr, &image.view), "create image view");
    }

    Image& target(int width, int height, VkFormat format) {
        if (width <= 0 || height <= 0 || width > lcomp::kMaxPreviewEdge ||
            height > lcomp::kMaxPreviewEdge)
            throw std::runtime_error("invalid composite dimensions");
        const auto bytes =
            static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 8;
        allocated += bytes;
        if (allocated > 512ull * 1024 * 1024)
            throw std::runtime_error("composite exceeds 512 MiB target budget");
        if (image_at == images.size())
            images.emplace_back();
        // Cached video frames change which scratch images are needed per tick.
        // Match unused resources by shape rather than rebuilding positional slots.
        const auto match = std::find_if(images.begin() + static_cast<std::ptrdiff_t>(image_at),
                                        images.end(), [&](const auto& image) {
                                            return image && image->width == width &&
                                                   image->height == height &&
                                                   image->format == format;
                                        });
        if (match != images.end())
            std::iter_swap(images.begin() + static_cast<std::ptrdiff_t>(image_at), match);
        auto& entry = images[image_at++];
        if (entry && entry->width == width && entry->height == height && entry->format == format)
            return *entry;
        entry = std::make_unique<Image>(*c);
        entry->width = width;
        entry->height = height;
        entry->format = format;
        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = format;
        ci.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
        ci.mipLevels = ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateImage(c->device, &ci, nullptr, &entry->image), "create composite image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(c->device, entry->image, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        mi.memoryTypeIndex =
            memory_type(*c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(c->device, &mi, nullptr, &entry->memory),
                "allocate composite image");
        checked(vkBindImageMemory(c->device, entry->image, entry->memory, 0),
                "bind composite image");
        view(*entry);
        return *entry;
    }

    void buffer(std::unique_ptr<Buffer>& b, VkDeviceSize size) {
        if (b && b->size >= size)
            return;
        b = std::make_unique<Buffer>(*c);
        b->size = size;
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateBuffer(c->device, &bi, nullptr, &b->buffer), "create staging buffer");
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(c->device, b->buffer, &req);
        VkMemoryAllocateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mi.allocationSize = req.size;
        try {
            mi.memoryTypeIndex = memory_type(*c, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        } catch (const std::runtime_error&) {
            mi.memoryTypeIndex = memory_type(*c, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        checked(vkAllocateMemory(c->device, &mi, nullptr, &b->memory), "allocate staging buffer");
        checked(vkBindBufferMemory(c->device, b->buffer, b->memory, 0), "bind staging buffer");
        checked(vkMapMemory(c->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped),
                "map staging buffer");
    }

    void barrier(Image& image, VkImageLayout old_layout, VkImageLayout next, bool acquire = false,
                 bool release = false) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = old_layout;
        b.newLayout = next;
        b.image = image.image;
        b.srcAccessMask = acquire || old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0u
                              : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b.dstAccessMask = release ? 0u : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        const auto external =
            c->have_foreign_queue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        b.srcQueueFamilyIndex = acquire   ? external
                                : release ? c->queue_family
                                          : VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = acquire   ? c->queue_family
                                : release ? external
                                          : VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    Image& source(const lcomp::DrawOp& op) {
        if (op.device_image) {
            const auto& dmabuf = *static_cast<const capture::DmabufImage*>(op.device_image);
            const auto handle = static_cast<VkImage>(dmabuf.image);
            auto& entry = foreign[handle];
            if (!entry) {
                entry = std::make_unique<Image>(*c);
                entry->image = handle;
                entry->owns = false;
                entry->width = static_cast<int>(dmabuf.width);
                entry->height = static_cast<int>(dmabuf.height);
                const auto format = dmabuf.drm_format;
                if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_XRGB8888 &&
                    format != DRM_FORMAT_ABGR8888 && format != DRM_FORMAT_XBGR8888)
                    throw std::runtime_error("unsupported captured GPU format");
                entry->format = format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888
                                    ? VK_FORMAT_B8G8R8A8_UNORM
                                    : VK_FORMAT_R8G8B8A8_UNORM;
                view(*entry, format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888);
            }
            if (std::ranges::find(acquired, entry.get()) == acquired.end()) {
                // Wayland delivers the slot; dma-buf's writer fence orders its GPU writes.
                pollfd ready{dmabuf.dmabuf_fd, POLLIN, 0};
                if (ready.fd < 0 || ::poll(&ready, 1, 2000) <= 0 || (ready.revents & POLLIN) == 0)
                    throw std::runtime_error("capture producer fence did not signal");
                barrier(*entry, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, true);
                acquired.push_back(entry.get());
            }
            return *entry;
        }
        const auto& pic = op.image;
        // ponytail: upload decoded stills each composite; add a texture cache if profiling
        // an image-heavy show puts uploads on the critical path.
        if (!pic.read_from())
            throw std::runtime_error("draw has no source pixels");
        auto& image = target(pic.width, pic.height,
                             op.bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM);
        if (upload_at == uploads.size())
            uploads.emplace_back();
        auto& stage = uploads[upload_at++];
        const auto bytes =
            static_cast<VkDeviceSize>(pic.width) * static_cast<VkDeviceSize>(pic.height) * 4;
        buffer(stage, bytes);
        std::memcpy(stage->mapped, pic.read_from(), bytes);
        barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(pic.width),
                            static_cast<std::uint32_t>(pic.height), 1};
        vkCmdCopyBufferToImage(cmd, stage->buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        barrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        return image;
    }

    void dispatch(Image& dest, Image& src, VkPipeline pipe, const Push& push, int width, int height,
                  Image* auxiliary = nullptr) {
        if (width <= 0 || height <= 0)
            return;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        checked(vkAllocateDescriptorSets(c->device, &ai, &set), "allocate composite descriptors");
        const VkDescriptorImageInfo infos[] = {
            {VK_NULL_HANDLE, dest.view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, src.view, VK_IMAGE_LAYOUT_GENERAL},
            {sampler, auxiliary ? auxiliary->view : src.view, VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < 3; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                              : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(c->device, 3, writes.data(), 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, (static_cast<std::uint32_t>(width) + 15) / 16,
                      (static_cast<std::uint32_t>(height) + 15) / 16, 1);
        barrier(dest, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    }

    Image& draw(const lcomp::DrawPlan& plan, bool transparent, unsigned depth = 0) {
        if (depth > 64)
            throw std::runtime_error("precomp nesting exceeds GPU limit");
        auto& dest = target(plan.width, plan.height, VK_FORMAT_R16G16B16A16_SFLOAT);
        barrier(dest, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue clear{};
        clear.float32[3] = transparent ? 0.0f : 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dest.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        barrier(dest, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        for (const auto& op : plan.ops) {
            const auto& r = op.rect;
            const int width = std::min(r.x1, plan.width) - std::max(r.x0, 0);
            const int height = std::min(r.y1, plan.height) - std::max(r.y0, 0);
            if (width <= 0 || height <= 0)
                continue;
            Push push;
            push.rect = {r.x0, r.y0, r.x1, r.y1};
            push.crop = op.crop;
            push.flags = {op.quarter, op.flip_x, op.flip_y,
                          !op.filters.empty() ? 2
                          : op.colour         ? 0
                          : op.nested         ? 2
                                              : 1};
            push.opacity[0] = static_cast<float>(std::clamp(op.opacity, 0.0, 1.0));
            push.opacity[1] = static_cast<float>(op.blend);
            Image* input = &dest;
            if (op.filters.size() == 1 && op.filters.front().enabled &&
                op.filters.front().type == "filter.scale" && !op.nested && !op.colour) {
                // Scale uploaded sRGB directly: avoid a linear intermediate and
                // converting every interpolation tap back to sRGB.
                const auto resolved = lcomp::resolve_filter(op.filters.front(), op.image.width,
                                                            op.image.height, plan.time_ns);
                auto cached = std::ranges::find_if(media_images, [&](const auto& entry) {
                    return entry.source == op.source_key && entry.filters == op.filters;
                });
                const auto bytes = static_cast<std::uint64_t>(resolved.width) *
                                   static_cast<std::uint64_t>(resolved.height) * 8;
                if (cached == media_images.end() && op.frame_owner && media_images.size() < 8 &&
                    media_bytes + bytes <= 128ull * 1024 * 1024) {
                    target(resolved.width, resolved.height, VK_FORMAT_R16G16B16A16_SFLOAT);
                    media_images.push_back(
                        {op.source_key, op.filters, {}, std::move(images[--image_at])});
                    media_bytes += bytes;
                    cached = std::prev(media_images.end());
                }
                const bool hit = cached != media_images.end() && cached->frame == op.frame_owner;
                input = cached != media_images.end() ? cached->image.get()
                                                     : &target(resolved.width, resolved.height,
                                                               VK_FORMAT_R16G16B16A16_SFLOAT);
                if (!hit) {
                    barrier(*input, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    Push packed;
                    packed.rect = {resolved.kind, 1, 0, 0};
                    std::memcpy(reinterpret_cast<char*>(&packed) + 16, resolved.values.data(),
                                sizeof(resolved.values));
                    dispatch(*input, source(op), filter, packed, resolved.width, resolved.height);
                    if (cached != media_images.end())
                        cached->frame = op.frame_owner;
                }
            } else if (!op.filters.empty()) {
                auto raw = op;
                raw.filters.clear();
                raw.opacity = 1;
                raw.blend = ldoc::BlendMode::Normal;
                raw.crop = {};
                raw.quarter = 0;
                raw.flip_x = raw.flip_y = false;
                const int w = op.nested ? op.nested->width : op.image.width;
                const int h = op.nested ? op.nested->height : op.image.height;
                raw.rect = {0, 0, w, h};
                lcomp::DrawPlan local;
                local.time_ns = plan.time_ns;
                local.width = w;
                local.height = h;
                local.ops.push_back(std::move(raw));
                local.filters = op.filters;
                input = &draw(local, true, depth + 1);
            } else if (op.colour) {
                const auto& c0 = *op.colour;
                push.colour = {static_cast<float>(c0.r), static_cast<float>(c0.g),
                               static_cast<float>(c0.b), static_cast<float>(c0.a)};
            } else {
                input = op.nested ? &draw(*op.nested, true, depth + 1) : &source(op);
            }
            dispatch(dest, *input, composite, push, width, height);
        }
        Image* result = &dest;
        std::vector<Image*> scratch{&dest};
        for (const auto& effect : plan.filters) {
            if (!effect.enabled ||
                (lcomp::filter_info(effect.type) && lcomp::filter_info(effect.type)->audio))
                continue;
            const auto resolved =
                lcomp::resolve_filter(effect, result->width, result->height, plan.time_ns);
            if (resolved.kind == 0)
                continue;
            if (resolved.kind == 13) {
                if (resolved.values[0] > 0)
                    result = &delayed(*result, resolved);
                continue;
            }
            const auto reuse = std::ranges::find_if(scratch, [&](const auto* image) {
                return image != result && image->width == resolved.width &&
                       image->height == resolved.height;
            });
            Image& output = reuse == scratch.end() ? target(resolved.width, resolved.height,
                                                            VK_FORMAT_R16G16B16A16_SFLOAT)
                                                   : **reuse;
            if (reuse == scratch.end())
                scratch.push_back(&output);
            barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            Push packed;
            static_assert(sizeof(packed) == sizeof(std::array<int, 4>) + sizeof(resolved.values));
            packed.rect = {resolved.kind, 0, 0, 0};
            std::memcpy(reinterpret_cast<char*>(&packed) + 16, resolved.values.data(),
                        sizeof(resolved.values));
            Image* auxiliary = nullptr;
            if (resolved.auxiliary) {
                const auto cached = std::ranges::find_if(auxiliaries, [&](const auto& item) {
                    return item.pixels == resolved.auxiliary;
                });
                if (cached != auxiliaries.end()) {
                    auxiliary = cached->image.get();
                } else {
                    lcomp::DrawOp mask;
                    mask.image.width = resolved.auxiliary->width;
                    mask.image.height = resolved.auxiliary->height;
                    mask.image.stride = resolved.auxiliary->stride;
                    mask.image.borrowed = resolved.auxiliary->read_from();
                    auxiliary = &source(mask);
                    const auto bytes = static_cast<std::uint64_t>(mask.image.width) *
                                       static_cast<std::uint64_t>(mask.image.height) * 8;
                    // Decode caches replace immutable pictures when files change. Keep at
                    // most eight / 128 MiB of GPU copies, counted in the target budget.
                    if (auxiliaries.size() < 8 && auxiliary_bytes + bytes <= 128ull * 1024 * 1024) {
                        auxiliaries.push_back(
                            {resolved.auxiliary, std::move(images[image_at - 1])});
                        auxiliary_bytes += bytes;
                    }
                }
            }
            dispatch(output, *result, filter, packed, resolved.width, resolved.height, auxiliary);
            result = &output;
        }
        return *result;
    }

    lcomp::Picture render(const lcomp::DrawPlan& plan, std::uint32_t max_edge) {
        const auto begin = std::chrono::steady_clock::now();
        lcomp::Picture out;
        out.width = plan.width;
        out.height = plan.height;
        out.status = plan.status;
        out.gaps = plan.gaps;
        if (plan.status == lcomp::PictureStatus::Unsupported)
            return out;
        std::vector<const lcomp::DrawOp*> media;
        const auto visit = [&](auto&& self, const lcomp::DrawPlan& current) -> void {
            for (const auto& op : current.ops) {
                if (op.frame_owner)
                    media.push_back(&op);
                if (op.nested)
                    self(self, *op.nested);
            }
        };
        visit(visit, plan);
        std::erase_if(media_images, [&](const auto& entry) {
            return std::ranges::none_of(media, [&](const auto* op) {
                return op->source_key == entry.source && op->filters == entry.filters;
            });
        });
        media_bytes = 0;
        for (const auto& entry : media_images)
            media_bytes += static_cast<std::uint64_t>(entry.image->width) *
                           static_cast<std::uint64_t>(entry.image->height) * 8;
        checked(vkResetCommandPool(c->device, pool, 0), "reset composite commands");
        checked(vkResetDescriptorPool(c->device, descriptors, 0), "reset composite descriptors");
        checked(vkResetFences(c->device, 1, &fence), "reset composite fence");
        image_at = upload_at = 0;
        std::erase_if(auxiliaries, [](const auto& item) { return item.pixels.use_count() == 1; });
        auxiliary_bytes = 0;
        for (const auto& item : auxiliaries)
            auxiliary_bytes += static_cast<std::uint64_t>(item.image->width) *
                               static_cast<std::uint64_t>(item.image->height) * 8;
        allocated = media_bytes + auxiliary_bytes;
        acquired.clear();
        recycled = std::move(retired);
        retired.clear();
        runtime_gaps.clear();
        std::erase_if(histories, [&](const auto& item) {
            return plan.time_ns > item.second.last &&
                   plan.time_ns - item.second.last > 21000000000ULL;
        });
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(cmd, &bi), "begin composite");
        Image& canvas = draw(plan, false);
        if (!runtime_gaps.empty()) {
            out.gaps.insert(out.gaps.end(), runtime_gaps.begin(), runtime_gaps.end());
            out.status = lcomp::PictureStatus::Partial;
        }
        const auto edge = static_cast<std::uint32_t>(std::max(plan.width, plan.height));
        if (max_edge && max_edge < edge) {
            out.width = std::max(
                1, static_cast<int>(static_cast<std::uint64_t>(plan.width) * max_edge / edge));
            out.height = std::max(
                1, static_cast<int>(static_cast<std::uint64_t>(plan.height) * max_edge / edge));
        }
        Image& reduced = target(out.width, out.height, VK_FORMAT_R8G8B8A8_UNORM);
        barrier(reduced, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        dispatch(reduced, canvas, preview, {}, out.width, out.height);
        out.stride = static_cast<std::uint32_t>(out.width) * 4;
        const auto bytes =
            static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height);
        buffer(readback, bytes);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<std::uint32_t>(out.width),
                            static_cast<std::uint32_t>(out.height), 1};
        vkCmdCopyImageToBuffer(cmd, reduced.image, VK_IMAGE_LAYOUT_GENERAL, readback->buffer, 1,
                               &copy);
        for (auto* image : acquired)
            barrier(*image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, false, true);
        VkMemoryBarrier host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &host, 0, nullptr, 0, nullptr);
        checked(vkEndCommandBuffer(cmd), "end composite");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        checked(vkQueueSubmit(c->queue, 1, &submit, fence), "submit composite");
        const auto submitted = std::chrono::steady_clock::now();
        checked(vkWaitForFences(c->device, 1, &fence, VK_TRUE, 2'000'000'000ULL),
                "GPU fence failed");
        if (std::getenv("LIBERTAS_BENCHMARK") && ++timing_frames_ % 120 == 0)
            std::fprintf(stderr, "GPU %dx%d prepare %.3f ms, wait %.3f ms\n", plan.width,
                         plan.height,
                         std::chrono::duration<double, std::milli>(submitted - begin).count(),
                         std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - submitted)
                             .count());
        out.pixels.resize(bytes);
        std::memcpy(out.pixels.data(), readback->mapped, bytes);
        std::uint64_t spare_bytes = 0;
        for (std::size_t i = image_at; i < images.size(); ++i)
            if (images[i])
                spare_bytes += static_cast<std::uint64_t>(images[i]->width) *
                               static_cast<std::uint64_t>(images[i]->height) * 8;
        while (images.size() > image_at && allocated + spare_bytes > 512ull * 1024 * 1024) {
            if (images.back())
                spare_bytes -= static_cast<std::uint64_t>(images.back()->width) *
                               static_cast<std::uint64_t>(images.back()->height) * 8;
            images.pop_back();
        }
        // Keep idle upload buffers for the next decoded frame; cache hits must
        // not free/reallocate mapped Vulkan memory on alternate ticks.
        VkDeviceSize staging_bytes = 0;
        for (const auto& stage : uploads)
            if (stage)
                staging_bytes += stage->size;
        while (uploads.size() > upload_at && staging_bytes > 128ull * 1024 * 1024) {
            staging_bytes -= uploads.back()->size;
            uploads.pop_back();
        }
        foreign.clear();
        acquired.clear();
        return out;
    }

    unsigned timing_frames_ = 0;
};

GpuCompositor::GpuCompositor() : impl_(std::make_unique<Impl>()) {}

GpuCompositor::~GpuCompositor() = default;

std::unique_ptr<GpuCompositor> GpuCompositor::create(VkCtx& device, std::string& error) {
    auto result = std::unique_ptr<GpuCompositor>(new GpuCompositor());
    result->impl_->c = &device;
    try {
        result->impl_->init();
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    return result;
}

std::optional<lcomp::Picture> GpuCompositor::render(const lcomp::DrawPlan& plan,
                                                    std::uint32_t max_edge, std::string& error) {
    try {
        return impl_->render(plan, max_edge);
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    }
}
} // namespace libertas::rhi
