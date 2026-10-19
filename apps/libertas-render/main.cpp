// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The render process owns capture and the device. Both backends consume lcomp's shared plan.
// Offscreen checks select CPU; normal runs try GPU and retain the CPU fallback (ADR-0042).

#include "lcomp/preview.hpp"
#include "lipc/fd.hpp"
#include "source_worker.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <lcapture/screen.hpp>
#include <lcapture/video.hpp>
#include <lcore/log.hpp>
#include <lrhi/composite.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

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

/// One capture snapshot per connector and commit, shared by every placement and canvas.
class Renderer {
  public:
    Renderer() {
        const char* mode = std::getenv("LIBERTAS_RENDER_BACKEND");
        require_gpu_ = mode && std::strcmp(mode, "gpu") == 0;
        const char* platform = std::getenv("QT_QPA_PLATFORM");
        cpu_ = (mode && std::strcmp(mode, "cpu") == 0) ||
               (!require_gpu_ && platform && std::strcmp(platform, "offscreen") == 0);
    }

    lcomp::Picture render(const ldoc::Project& project, const ldoc::Id& canvas,
                          std::uint32_t max_edge) {
        if (!cpu_ && !gpu_failed_ && !gpu_) {
            if (device()) {
                std::string error;
                gpu_ = libertas::rhi::GpuCompositor::create(*device_, error);
                if (!gpu_)
                    fail_gpu(error);
                else
                    lc::info("GPU composite: {}", libertas::rhi::vk_device_name(*device_));
            } else
                fail_gpu("no usable Vulkan device");
        }
        auto frames = [this](const ldoc::Source& source) { return source_frame(source, bool(gpu_)); };
        auto plan = lcomp::plan_canvas(project, canvas, frames);
        if (gpu_) {
            std::string error;
            auto result = gpu_->render(plan, max_edge, error);
            if (result)
                return std::move(*result);
            // Work still in flight cannot have its resources or capture leases recycled.
            // Exiting lets the OS reclaim them; the UI reports the lost process.
            if (error.starts_with("GPU fence failed")) {
                lc::error("{}; stopping renderer", error);
                std::_Exit(1);
            }
            fail_gpu(error);
            gpu_.reset();
            plan = lcomp::plan_canvas(project, canvas, [this](const ldoc::Source& source) {
                return source_frame(source, false);
            });
        }
        return lcomp::preview_size(lcomp::execute_cpu(plan), max_edge);
    }

    bool fresh() const {
        for (const auto& [id, session] : workers_) {
            std::lock_guard lock(session->mu);
            if (session->sequence != session->drawn) return true;
        }
        for (const auto& [id, session] : videos_) {
            std::lock_guard lock(session->mu);
            if (session->sequence != session->drawn) return true;
        }
        for (const auto& [name, session] : sessions_) {
            std::lock_guard lock(session->mu);
            if (session->sequence != session->drawn)
                return true;
        }
        return false;
    }

    void done() {
        for (const auto& [id, snapshot] : worker_held_) {
            auto& session = *workers_.at(id);
            std::lock_guard lock(session.mu);
            session.drawn = snapshot.second;
        }
        worker_held_.clear();
        std::erase_if(workers_, [this](const auto& item) { return !worker_wanted_.contains(item.first); });
        worker_wanted_.clear();
        for (const auto& [id, snapshot] : video_held_) {
            auto& session = *videos_.at(id);
            std::lock_guard lock(session.mu);
            session.drawn = snapshot.second;
        }
        video_held_.clear();
        std::erase_if(videos_, [this](const auto& item) { return !video_wanted_.contains(item.first); });
        video_wanted_.clear();
        for (auto& [name, snapshot] : held_) {
            auto& s = *sessions_.at(name);
            std::lock_guard lock(s.mu);
            s.drawn = snapshot.sequence;
        }
        held_.clear();
        std::erase_if(sessions_,
                      [this](const auto& entry) { return !wanted_.contains(entry.first); });
        wanted_.clear();
    }

  private:
    struct WorkerSession {
        ldoc::Source source;
        std::mutex mu;
        std::shared_ptr<const lcomp::PreviewFrame> last;
        std::uint64_t sequence = 0, drawn = 0;
        std::unique_ptr<SourceWorker> worker;
        ~WorkerSession() { worker.reset(); }
    };
    std::map<std::string, std::unique_ptr<WorkerSession>> workers_;
    std::map<std::string, std::pair<std::shared_ptr<const lcomp::PreviewFrame>, std::uint64_t>> worker_held_;
    std::set<std::string> worker_wanted_;

    lcomp::CapturedPicture worker_frame(const ldoc::Source& source) {
        const auto id = source.id.to_string();
        worker_wanted_.insert(id);
        auto existing = workers_.find(id);
        if (existing != workers_.end() &&
            (existing->second->source.settings != source.settings || existing->second->source.type != source.type)) {
            worker_held_.erase(id);
            workers_.erase(existing);
        }
        if (!workers_.contains(id)) {
            auto session = std::make_unique<WorkerSession>();
            session->source = source;
            auto* ptr = session.get();
            session->worker = std::make_unique<SourceWorker>(source, [ptr](auto frame) {
                std::lock_guard lock(ptr->mu);
                ptr->last = std::move(frame);
                ++ptr->sequence;
            });
            workers_.emplace(id, std::move(session));
        }
        if (!worker_held_.contains(id)) {
            auto& session = *workers_.at(id);
            std::lock_guard lock(session.mu);
            worker_held_.emplace(id, std::make_pair(session.last, session.sequence));
        }
        const auto& image = worker_held_.at(id).first;
        if (!image) return {};
        return {image->width, image->height, image->pixels.data(), image->pixels.size(), nullptr, false};
    }

    struct VideoSession {
        ldoc::Source source;
        std::mutex mu;
        std::shared_ptr<const libertas::capture::VideoFrame> last;
        std::uint64_t sequence = 0, drawn = 0;
        std::unique_ptr<libertas::capture::VideoCapture> capture;
        ~VideoSession() { if (capture) capture->stop(); }
    };
    std::map<std::string, std::unique_ptr<VideoSession>> videos_;
    std::map<std::string, std::pair<std::shared_ptr<const libertas::capture::VideoFrame>, std::uint64_t>> video_held_;
    std::set<std::string> video_wanted_;

    static std::string setting(const ldoc::Source& source, const std::string& key) {
        const auto it = source.settings.find(key);
        if (it != source.settings.end())
            if (const auto* text = std::get_if<std::string>(&it->second)) return *text;
        return {};
    }
    lcomp::CapturedPicture source_frame(const ldoc::Source& source, bool gpu) {
        if (source.type == "source.text" || source.type == "source.media" ||
            source.type == "source.slideshow" || source.type == "source.playlist" || source.type == "source.browser")
            return worker_frame(source);
        if (source.type != "source.v4l2") return frame(setting(source, "output"), gpu);
        const auto id = source.id.to_string();
        video_wanted_.insert(id);
        auto existing = videos_.find(id);
        if (existing != videos_.end() && existing->second->source.settings != source.settings) {
            video_held_.erase(id);
            videos_.erase(existing);
        }
        if (!videos_.contains(id)) {
            auto session = std::make_unique<VideoSession>();
            session->source = source;
            std::string error;
            session->capture = libertas::capture::VideoCapture::open(setting(source, "device"),
                setting(source, "resolution"), setting(source, "frameRate"), setting(source, "colourRange"), error);
            if (!session->capture) lc::warn("cannot capture {}: {}", setting(source, "device"), error);
            else {
                auto* ptr = session.get();
                session->capture->start([ptr](auto frame) {
                    std::lock_guard lock(ptr->mu);
                    ptr->last = std::move(frame);
                    ++ptr->sequence;
                });
            }
            videos_.emplace(id, std::move(session));
        }
        if (!video_held_.contains(id)) {
            auto& session = *videos_.at(id);
            std::lock_guard lock(session.mu);
            video_held_.emplace(id, std::make_pair(session.last, session.sequence));
        }
        const auto& image = video_held_.at(id).first;
        if (!image) return {};
        return {image->width, image->height, image->pixels.data(), image->pixels.size(), nullptr};
    }

    struct Session {
        std::mutex mu;
        std::shared_ptr<const libertas::capture::DmabufImage> last;
        std::uint64_t sequence = 0, drawn = 0;
        std::unique_ptr<libertas::capture::ScreenCapture> capture;

        ~Session() {
            if (capture)
                capture->stop();
        }
    };

    struct Snapshot {
        std::shared_ptr<const libertas::capture::DmabufImage> image;
        std::uint64_t sequence = 0;
        std::vector<std::uint8_t> pixels;
    };

    libertas::rhi::VkCtxPtr device_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::map<std::string, Snapshot> held_;
    std::set<std::string> wanted_;
    // Destroy GPU views before capture images, and capture images before their device.
    std::unique_ptr<libertas::rhi::GpuCompositor> gpu_;
    bool cpu_ = false, require_gpu_ = false, device_failed_ = false, gpu_failed_ = false,
         capture_failed_ = false;

    void fail_gpu(const std::string& error) {
        lc::warn("GPU composite unavailable: {}; using CPU", error);
        gpu_failed_ = true;
        capture_failed_ = error.find("(-4)") != std::string::npos;
        if (require_gpu_)
            std::_Exit(1);
    }

    bool device() {
        if (device_ || device_failed_)
            return bool(device_);
        const char* selected = std::getenv("LIBERTAS_RENDER_DEVICE");
        std::string error;
        device_ = libertas::rhi::vk_init(selected ? selected : "", error);
        if (!device_) {
            lc::warn("no capture device: {}", error);
            device_failed_ = true;
        }
        return bool(device_);
    }

    lcomp::CapturedPicture frame(const std::string& connector, bool gpu) {
        if (capture_failed_)
            return {};
        wanted_.insert(connector);
        if (!sessions_.contains(connector)) {
            auto session = std::make_unique<Session>();
            if (device()) {
                std::string error;
                session->capture =
                    libertas::capture::ScreenCapture::open(*device_, connector, true, error);
                if (!session->capture)
                    lc::warn("cannot capture {}: {}", connector, error);
                else {
                    Session* ptr = session.get();
                    session->capture->start_images([ptr](auto image) {
                        std::lock_guard lock(ptr->mu);
                        ptr->last = std::move(image);
                        ++ptr->sequence;
                    });
                }
            }
            sessions_.emplace(connector, std::move(session));
        }
        if (!held_.contains(connector)) {
            auto& session = *sessions_.at(connector);
            std::lock_guard lock(session.mu);
            held_.emplace(connector, Snapshot{session.last, session.sequence, {}});
        }
        auto& snap = held_.at(connector);
        if (!snap.image)
            return {};
        lcomp::CapturedPicture result;
        result.width = static_cast<int>(snap.image->width);
        result.height = static_cast<int>(snap.image->height);
        if (gpu)
            result.device_image = snap.image.get();
        else {
            if (snap.pixels.empty()) {
                std::string error;
                if (!libertas::rhi::vk_readback_exported(*device_, *snap.image, true, snap.pixels,
                                                         error)) {
                    lc::warn("capture readback failed: {}", error);
                    return {};
                }
            }
            result.pixels = snap.pixels.data();
            result.size = snap.pixels.size();
        }
        return result;
    }
};

} // namespace

int main() {
    const int fd = channel_fd();
    if (fd < 0) {
        std::fprintf(stderr, "libertas-render: LIBERTAS_CHANNEL_FD is not a descriptor\n");
        return 2;
    }

    Renderer renderer;
    auto served = lcomp::serve_preview(
        lipc::Channel{lipc::Fd{fd}}, {}, [&renderer] { renderer.done(); },
        [&renderer] { return renderer.fresh(); },
        [&renderer](const ldoc::Project& p, const ldoc::Id& id, std::uint32_t edge) {
            return renderer.render(p, id, edge);
        });
    if (!served) {
        std::fprintf(stderr, "libertas-render: %s\n", served.error().c_str());
        return 1;
    }
    return 0;
}
