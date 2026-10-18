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
#include <mutex>
#include <vector>
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
        Session& session = *it->second;
        if (!session.capture)
            return {};

        // Locked for the whole composite, not just for the handover: the picture that comes
        // back points into `last`, so the capture thread must not swap it mid-blit. The lock is
        // released in `done()` once the walk has finished with every frame it borrowed.
        session.mu.lock();
        held_.push_back(&session);
        if (session.last.empty())
            return {};

        lcomp::CapturedPicture out;
        out.width = static_cast<std::int32_t>(session.capture->width());
        out.height = static_cast<std::int32_t>(session.capture->height());
        out.pixels = session.last.data();
        out.size = session.last.size();
        return out;
    }

    /// Releases every frame the last composite borrowed. Called when the walk is finished.
    void done() {
        for (Session* s : held_)
            s->mu.unlock();
        held_.clear();
    }

    /// Whether any capture has produced a frame that no composite has drawn yet.
    ///
    /// The preview is asked to republish at the canvas's rate, but a 30 Hz screen on a 60 fps
    /// canvas has nothing new half the time, and compositing it again burns a full scale and
    /// blit to produce an identical picture. Asked before the walk rather than diffed after it.
    [[nodiscard]] bool anything_new() const {
        for (const auto& [name, session] : sessions_) {
            std::lock_guard<std::mutex> lock(session->mu);
            if (session->fresh)
                return true;
        }
        return false;
    }

    /// Marks every frame as drawn. Called once a composite has used them.
    void mark_drawn() {
        for (auto& [name, session] : sessions_) {
            std::lock_guard<std::mutex> lock(session->mu);
            session->fresh = false;
        }
    }

    [[nodiscard]] bool capturing() const { return !sessions_.empty(); }

private:
    struct Session {
        std::unique_ptr<libertas::capture::ScreenCapture> capture;
        /// The newest frame the capture thread has delivered, and the lock that lets the
        /// compositing thread read it without waiting for one.
        ///
        /// This replaces a synchronous read with a deadline, which ADR-0013 Consequence 4 says
        /// outright must not be done: a bounded wait that is re-requested makes the compositor
        /// answer at once with unchanged content, so the preview was measuring its own 120 ms
        /// timeout and calling it a frame rate. Every tick waited the full budget and returned
        /// nothing, which is the 8fps ceiling the operator saw.
        ///
        /// Only the newest frame is kept. A preview that fell behind should skip to what is on
        /// the screen now, not work through a queue of what used to be.
        mutable std::mutex mu;
        std::vector<std::uint8_t> last;
        /// Set when the capture thread writes a frame, cleared when a composite draws it.
        bool fresh = false;
    };

    std::unique_ptr<Session> open(const std::string& connector) {
        auto session = std::make_unique<Session>();
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

        session->capture =
            libertas::capture::ScreenCapture::open(*device_, connector, true, error);
        if (!session->capture) {
            lc::warn("cannot capture {}: {}", connector.empty() ? "the first screen" : connector,
                     error);
            return session;
        }
        // The thread blocks until the screen changes, which is what the protocol is built for:
        // a still desktop costs nothing, and a busy one delivers as fast as it redraws. The
        // composite never waits on it -- it takes whatever was last put here.
        Session* raw = session.get();
        session->capture->start([raw](std::vector<std::uint8_t>&& pixels) {
            std::lock_guard<std::mutex> lock(raw->mu);
            // Moved, not copied: 33 MB a frame at 4K, which measured 2.6 ms against 1.6 ms for
            // the scaling it feeds. The thread allocates the next one.
            raw->last = std::move(pixels);
            raw->fresh = true;
        });
        return session;
    }

    libertas::rhi::VkCtxPtr owned_device_;
    libertas::rhi::VkCtx* device_ = nullptr;
    bool device_failed_ = false;
    // unique_ptr because a Session owns a mutex and a running thread that holds a pointer to
    // it: the map must not move one once a capture has started.
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    /// Sessions whose lock the current composite is holding, released together in done().
    std::vector<Session*> held_;
};

} // namespace

int main() {
    const int fd = channel_fd();
    if (fd < 0) {
        std::fprintf(stderr, "libertas-render: LIBERTAS_CHANNEL_FD is not a descriptor\n");
        return 2;
    }

    Screens screens;
    auto served = lcomp::serve_preview(
        lipc::Channel{lipc::Fd{fd}},
        [&screens](const std::string& connector) { return screens.frame(connector); },
        [&screens] {
            screens.done();
            screens.mark_drawn();
        },
        [&screens] { return screens.anything_new(); });
    if (!served) {
        std::fprintf(stderr, "libertas-render: %s\n", served.error().c_str());
        return 1;
    }
    return 0;
}
