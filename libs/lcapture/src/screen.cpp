// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One screen, captured into a ring of our own buffers and read back as RGBA8.
//
// The sequence here is `capture-spike`'s, with the measuring removed. Keeping it in one place
// matters because the order is not obvious and most of it is only discoverable by failing:
// the session has to exist before the compositor will say what it can deliver, the modifier
// intersection is the whole negotiation (we allocate, so there is no second round), and the
// buffers have to be registered as slots before any of them can be captured into.

#include <lcapture/screen.hpp>

#include <lrhi/vulkan.hpp>

#include <lcore/log.hpp>

#include <algorithm>
#include <format>

namespace libertas::capture {

namespace {

/// BGRA as the compositor delivers it, RGBA as `lcomp` samples it. One swap over the buffer,
/// in place: the alternative is teaching the scene walk a second pixel order, and a preview
/// that disagrees with itself about channel order is a bug that looks like a colour decision.
void bgra_to_rgba(std::vector<std::uint8_t>& pixels) {
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4)
        std::swap(pixels[i], pixels[i + 2]);
}

} // namespace

struct ScreenCapture::Impl {
    rhi::VkCtx* device = nullptr;
    WlCapPtr cap;
    std::vector<rhi::VkExportedImage> ring;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slot = 0;

    ~Impl() {
        // The wl_buffers go with the connection; the images are ours.
        for (rhi::VkExportedImage& img : ring)
            if (device != nullptr)
                rhi::vk_destroy_exportable(*device, img);
    }
};

ScreenCapture::ScreenCapture() : impl_(std::make_unique<Impl>()) {}
ScreenCapture::~ScreenCapture() = default;

std::uint32_t ScreenCapture::width() const { return impl_->width; }
std::uint32_t ScreenCapture::height() const { return impl_->height; }

std::unique_ptr<ScreenCapture> ScreenCapture::open(rhi::VkCtx& device, const std::string& connector,
                                                   bool with_cursor, std::string& error) {
    auto self = std::unique_ptr<ScreenCapture>(new ScreenCapture());
    self->impl_->device = &device;

    self->impl_->cap = wlcap_open(error);
    if (!self->impl_->cap)
        return nullptr;

    WlCapCaps caps;
    if (!wlcap_begin_session(*self->impl_->cap, connector, with_cursor, caps, error))
        return nullptr;
    self->impl_->width = caps.width;
    self->impl_->height = caps.height;

    // Four, for the reason the header gives: one buffer serialises the compositor against us.
    constexpr std::uint32_t kSlots = 4;

    for (const WlCapFormat& format : caps.formats) {
        const std::vector<std::uint64_t> ours =
            rhi::vk_supported_modifiers(device, format.drm_format);
        std::vector<std::uint64_t> common;
        for (std::uint64_t m : format.modifiers)
            if (std::find(ours.begin(), ours.end(), m) != ours.end())
                common.push_back(m);
        if (common.empty())
            continue;

        std::vector<rhi::VkExportedImage> ring(kSlots);
        bool ok = true;
        for (std::uint32_t i = 0; i < kSlots && ok; ++i) {
            // host_memory false: the compositor and we are on the same device here. A second GPU
            // would need the system-RAM round trip ADR-0019 measured, and that is an encode-path
            // decision rather than a preview one.
            if (!rhi::vk_create_exportable(device, caps.width, caps.height, format.drm_format,
                                           common, false, ring[i], error))
                ok = false;
            else if (!wlcap_add_slot(*self->impl_->cap, ring[i], format.drm_format, error))
                ok = false;
        }
        if (!ok) {
            for (rhi::VkExportedImage& img : ring)
                rhi::vk_destroy_exportable(device, img);
            continue;
        }
        self->impl_->ring = std::move(ring);
        lc::info("capture: {}x{} {} on {}", caps.width, caps.height,
                    fourcc_str(format.drm_format), connector.empty() ? "the first screen" : connector);
        return self;
    }

    // Every format the compositor offered was one the device cannot allocate with. Naming both
    // sides, because this is the failure that looks like "capture is broken" and is actually a
    // driver and compositor that have no modifier in common.
    error = std::format("no format the compositor offers ({}) can be allocated on this device",
                        caps.formats.size());
    return nullptr;
}

bool ScreenCapture::read_frame(std::vector<std::uint8_t>& out, std::uint32_t timeout_ms, bool& idle,
                               std::string& error) {
    idle = false;
    if (impl_->ring.empty()) {
        error = "capture has no buffers";
        return false;
    }
    const std::size_t index = impl_->slot % impl_->ring.size();
    if (!wlcap_capture_slot(*impl_->cap, index, impl_->width, impl_->height, nullptr, error,
                            timeout_ms, &idle))
        return false;
    impl_->slot = (impl_->slot + 1) % static_cast<std::uint32_t>(impl_->ring.size());

    // `true`: the compositor wrote these pixels, not us, so the barrier has to acquire queue
    // ownership. Getting this wrong reads a correct-looking buffer of stale content.
    if (!rhi::vk_readback_exported(*impl_->device, impl_->ring[index], true, out, error))
        return false;
    bgra_to_rgba(out);
    return true;
}

} // namespace libertas::capture
