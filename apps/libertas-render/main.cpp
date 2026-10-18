// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The preview process. It composites the document it is sent and publishes one frame per canvas.
//
// It captures screens when the document asks for one, which is where the GPU arrives in this
// process -- ADR-0005 always said that is where it lives. It still does not encode or output.
//
// The device is opened on the first screen source and never before, so a document with no screen
// in it runs exactly as it did: no Vulkan instance, no compositor connection, and the headless
// self-check keeps working on a machine with neither (ADR-0040 required that, and it still
// holds -- the check's scenes have no screen sources).

#include "lcomp/preview.hpp"
#include "lipc/fd.hpp"

#include <lcapture/screen.hpp>
#include <lrhi/vulkan.hpp>

#include <lcore/log.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int channel_fd() {
    const char* spec = std::getenv("LIBERTAS_CHANNEL_FD");
    if (spec == nullptr || *spec == '\0')
        return -1;
    char* end = nullptr;
    errno = 0;
    const long fd = std::strtol(spec, &end, 10);
    if (errno != 0 || end == spec || *end != '\0' || fd < 0 || fd > 1'000'000L)
        return -1;
    return static_cast<int>(fd);
}

/// Opens the device once, and one capture session per screen the document names.
///
/// Lazy on purpose, twice over. The device is not opened until a screen source asks for a frame,
/// so a document without one costs nothing and still runs where there is no GPU. A session is not
/// opened until its connector is asked for, so a project naming a screen that is unplugged draws
/// the rest of the scene and reports that one as a gap.
///
/// A failure is remembered rather than retried every composite. Retrying would reopen a Wayland
/// connection on every frame for a screen that is not coming back, and the caption already says
/// the source is not drawn; a loop that fails faster does not say it better.
class Screens {
public:
    lcomp::CapturedPicture frame(const std::string& connector) {
        auto it = sessions_.find(connector);
        if (it == sessions_.end())
            it = sessions_.emplace(connector, open(connector)).first;
        Session& session = it->second;
        if (!session.capture)
            return {};

        bool idle = false;
        std::string error;
        // Long enough for a screen that is redrawing to deliver, short enough that a still one
        // does not hold the composite. A first attempt passed 1 ms, which was wrong twice over:
        // no frame can arrive in that window, so every capture reported idle and the source was
        // never drawn -- the caption said "1 not drawn" over a working capture session. ADR-0013
        // also records that abandoning a pending frame makes the compositor answer the *next*
        // request at once with unchanged content, so a tight poll invents frames the screen never
        // drew. This waits once, properly, and accepts that a genuinely idle desktop costs that
        // wait. An idle screen then redraws with its last frame; one that has never drawn is a
        // gap until it does.
        std::vector<std::uint8_t> pixels;
        if (session.capture->read_frame(pixels, 120, idle, error)) {
            session.last = std::move(pixels);
        } else if (!idle && !error.empty() && error != session.last_error) {
            lc::warn("capture on {}: {}", connector.empty() ? "the first screen" : connector, error);
            session.last_error = error;
        }
        if (session.last.empty())
            return {};

        lcomp::CapturedPicture out;
        out.width = static_cast<std::int32_t>(session.capture->width());
        out.height = static_cast<std::int32_t>(session.capture->height());
        out.pixels = session.last;
        return out;
    }

private:
    struct Session {
        std::unique_ptr<libertas::capture::ScreenCapture> capture;
        /// The last frame that arrived. Kept because a still screen sends nothing and the preview
        /// should go on showing what is on it rather than blinking to a gap.
        std::vector<std::uint8_t> last;
        std::string last_error;
    };

    Session open(const std::string& connector) {
        Session session;
        std::string error;
        if (device_ == nullptr && !device_failed_) {
            // Empty: let the device be chosen the way vk_init chooses it. Naming a DRM node here
            // would be this process deciding which GPU the show runs on, which is a setting and
            // not a default (ADR-0008 forbids choosing by enumeration index, not by policy).
            owned_device_ = libertas::rhi::vk_init(std::string{}, error);
            if (!owned_device_) {
                lc::warn("no vulkan device, so no screen can be captured: {}", error);
                device_failed_ = true;
            } else {
                device_ = owned_device_.get();
                lc::info("capture device: {}", libertas::rhi::vk_device_name(*device_));
            }
        }
        if (device_ == nullptr)
            return session;

        session.capture =
            libertas::capture::ScreenCapture::open(*device_, connector, true, error);
        if (!session.capture)
            lc::warn("cannot capture {}: {}", connector.empty() ? "the first screen" : connector,
                     error);
        return session;
    }

    libertas::rhi::VkCtxPtr owned_device_;
    libertas::rhi::VkCtx* device_ = nullptr;
    bool device_failed_ = false;
    std::map<std::string, Session> sessions_;
};

} // namespace

int main() {
    const int fd = channel_fd();
    if (fd < 0) {
        std::fprintf(stderr, "libertas-render: LIBERTAS_CHANNEL_FD is not a descriptor\n");
        return 2;
    }

    Screens screens;
    auto served = lcomp::serve_preview(lipc::Channel{lipc::Fd{fd}},
                                       [&screens](const std::string& connector) {
                                           return screens.frame(connector);
                                       });
    if (!served) {
        std::fprintf(stderr, "libertas-render: %s\n", served.error().c_str());
        return 1;
    }
    return 0;
}
