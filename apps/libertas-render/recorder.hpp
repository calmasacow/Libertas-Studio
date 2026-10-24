// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio_sources.hpp"

#include <lcomp/composite.hpp>
#include <lcomp/preview.hpp>
#include <lcore/log.hpp>
#include <ldoc/document.hpp>
#include <lencode/encoder.hpp>

#include <chrono>
#include <string>

/// Writing one encode to one file.
///
/// One at a time, deliberately. The encoder ladder picks a real device (ADR-0006) and two
/// recordings would contend for it; the shape that wants several is several encodes going to
/// several destinations, which is the streaming path and is not built yet.
///
/// Video and audio are stamped off one clock -- the wall clock at the first frame -- because
/// ADR-0017 does not care which clock the muxer is given, only that there is exactly one.
class Recorder {
  public:
    explicit Recorder(AudioSources& audio) : audio_(audio) {}

    ~Recorder() { stop(); }

    [[nodiscard]] bool running() const { return encoder_ != nullptr; }

    /// Which canvas is being recorded, or nil. The preview loop asks this to decide whether a
    /// canvas has to be composited at full size as well as preview size.
    [[nodiscard]] const ldoc::Id& canvas() const { return canvas_; }

    lcomp::RecordState control(const lcomp::RecordRequest& request,
                               const ldoc::Project& project) {
        if (!request.start) {
            stop();
            return {};
        }
        if (encoder_) {
            // Not an error worth refusing over: the operator asked for a recording and will
            // get one. Rolling the file over is the behaviour that matches the button.
            stop();
        }

        const ldoc::Encode* encode = project.encode(request.encode);
        if (!encode)
            return {false, "that encode is not in this project"};
        const ldoc::Canvas* canvas = project.canvas(encode->canvas);
        if (!canvas)
            return {false, std::format("encode '{}' has no canvas to record", encode->name)};

        // Whichever audio encode a destination fed by this video encode names. Without one
        // the file is silent, which is a thing someone can mean and so is not refused.
        const ldoc::AudioEncode* audio = nullptr;
        for (const ldoc::Output& o : project.outputs) {
            if (o.encode == encode->id && o.audio_encode) {
                audio = project.audio_encode(*o.audio_encode);
                if (audio)
                    break;
            }
        }
        ldoc::AudioEncode silent;
        silent.bitrate_kbps = 0;

        auto cfg = libertas::encode::runtime_config(*encode, audio ? *audio : silent,
                                                    *canvas, request.path);
        if (!audio) {
            cfg.audio_channels = 0; // no audio stream at all, rather than a silent one
            cfg.audio_bitrate = 0;
        }

        std::string error;
        encoder_ = libertas::encode::encoder_open(cfg, error);
        if (!encoder_) {
            canvas_ = ldoc::Id::nil();
            audio_.tap(ldoc::Id::nil());
            return {false, error.empty() ? std::string{"the encoder would not open"} : error};
        }
        canvas_ = canvas->id;
        width_ = cfg.source_width;
        height_ = cfg.source_height;
        started_ = std::chrono::steady_clock::now();
        audio_.tap(audio ? audio->bus : ldoc::Id::nil());
        lc::info("recording {} to {}", encode->name, request.path);
        return {true, {}};
    }

    /// One composited frame at the canvas's own size.
    void frame(const ldoc::Id& canvas, const lcomp::Picture& picture) {
        if (!encoder_ || canvas != canvas_)
            return;
        // A frame that is not the size the encoder opened with cannot be submitted; the
        // encoder would have to be reopened, and a file that changes size mid-stream is not
        // one that plays. Dropping is the honest failure, and it says so once.
        if (picture.width != static_cast<std::int32_t>(width_) ||
            picture.height != static_cast<std::int32_t>(height_)) {
            if (!warned_size_) {
                warned_size_ = true;
                lc::warn("the canvas is {}x{} but the recording opened at {}x{}; frames are "
                         "being dropped",
                         picture.width, picture.height, width_, height_);
            }
            return;
        }
        const std::uint8_t* pixels = picture.read_from();
        if (pixels == nullptr)
            return;

        const auto now = std::chrono::steady_clock::now();
        const auto pts = std::chrono::duration_cast<std::chrono::nanoseconds>(now - started_)
                             .count();
        std::string error;
        if (!libertas::encode::encoder_video(*encoder_, pixels, picture.stride, pts, error)) {
            lc::error("recording stopped: {}", error);
            stop();
            return;
        }
        pump_audio(pts);
    }

    /// Flush and write the trailer. A file is not playable before this, so it happens on every
    /// path out -- an explicit stop, a failed frame, and the destructor.
    void stop() {
        if (!encoder_)
            return;
        pump_audio(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - started_)
                       .count());
        std::string error;
        if (!libertas::encode::encoder_finish(*encoder_, error))
            lc::error("the recording did not close cleanly: {}", error);
        else
            lc::info("recording finished");
        encoder_.reset();
        canvas_ = ldoc::Id::nil();
        warned_size_ = false;
        audio_.tap(ldoc::Id::nil());
    }

  private:
    void pump_audio(std::int64_t pts_ns) {
        auto samples = audio_.drain_tap();
        if (samples.empty())
            return;
        std::string error;
        const auto frames = static_cast<std::uint32_t>(samples.size() / 2);
        if (!libertas::encode::encoder_audio(*encoder_, samples.data(), frames, pts_ns, error))
            lc::warn("audio was not accepted by the recording: {}", error);
    }

    AudioSources& audio_;
    libertas::encode::EncoderPtr encoder_;
    ldoc::Id canvas_ = ldoc::Id::nil();
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::chrono::steady_clock::time_point started_{};
    bool warned_size_ = false;
};
