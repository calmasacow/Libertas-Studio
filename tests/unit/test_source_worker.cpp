// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../apps/libertas-render/source_worker.hpp"
#include "ltest.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace {
ldoc::Source source(const char* type) {
    ldoc::Source out;
    out.id = ldoc::Id::generate();
    out.type = type;
    out.name = "worker check";
    out.settings["width"] = std::int64_t{64};
    out.settings["height"] = std::int64_t{32};
    return out;
}

std::vector<lcomp::PreviewFrame> capture(const ldoc::Source& src, int count = 1) {
    std::mutex mu;
    std::condition_variable ready;
    std::vector<lcomp::PreviewFrame> frames;
    {
        SourceWorker worker(src, [&](auto frame) {
            std::lock_guard lock(mu);
            if (frame && (frames.empty() || frames.back().pixels != frame->pixels)) {
                frames.push_back(*frame);
                ready.notify_all();
            }
        });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8),
                       [&] { return static_cast<int>(frames.size()) >= count; });
    }
    return frames;
}
} // namespace

LTEST_CASE(text_worker_publishes_real_glyphs_with_transparency) {
    auto src = source("source.text");
    src.settings["text"] = std::string("Hi");
    src.settings["size"] = std::int64_t{22};
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    CHECK_EQ(frames[0].width, 64);
    bool ink = false, clear = false;
    for (std::size_t i = 3; i < frames[0].pixels.size(); i += 4) {
        ink = ink || frames[0].pixels[i] > 0;
        clear = clear || frames[0].pixels[i] == 0;
    }
    CHECK(ink);
    CHECK(clear);
}

LTEST_CASE(slideshow_worker_advances_between_real_images) {
    auto src = source("source.slideshow");
    src.settings["files"] =
        std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png\n" + LIBERTAS_IMAGE_FIXTURES + "/red.jpg";
    src.settings["intervalMs"] = std::int64_t{100};
    auto frames = capture(src, 2);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].pixels != frames[1].pixels);
}

LTEST_CASE(media_worker_decodes_video_in_its_process) {
    auto src = source("source.media");
    src.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion.mkv";
    auto frames = capture(src, 2);
    REQUIRE(frames.size() >= 2);
    CHECK(frames[0].pixels != frames[1].pixels);
}

LTEST_CASE(browser_worker_renders_local_html) {
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser.html";
    auto frames = capture(src);
    REQUIRE(!frames.empty());
    const auto& pixels = frames[0].pixels;
    CHECK(std::abs(int(pixels[0]) - 220) <= 2);
    CHECK(std::abs(int(pixels[1]) - 30) <= 2);
    const std::size_t background = (20 * 64 + 30) * 4;
    CHECK(std::abs(int(pixels[background]) - 20) <= 2);
    CHECK(std::abs(int(pixels[background + 2]) - 160) <= 2);
}

LTEST_CASE(unavailable_blackmagic_worker_exits_without_inventing_frames) {
    auto src = source("source.blackmagic");
    src.settings["deviceIndex"] = std::int64_t{999};
    std::mutex mu;
    std::condition_variable done;
    bool stopped = false, picture = false;
    {
        SourceWorker worker(src, [&](auto frame) {
            std::lock_guard lock(mu);
            picture = picture || bool(frame);
            stopped = !frame;
            done.notify_all();
        });
        std::unique_lock lock(mu);
        done.wait_for(lock, std::chrono::seconds(4), [&] { return stopped; });
        CHECK(stopped);
        CHECK(!picture);
    }
}

LTEST_CASE(browser_audio_reaches_pcm_transport) {
    if (!std::getenv("LIBERTAS_TEST_BROWSER_AUDIO")) return;
    auto src = source("source.browser");
    src.settings["url"] = std::string("file://") + LIBERTAS_IMAGE_FIXTURES + "/browser-tone.html";
    std::mutex mu;
    std::condition_variable ready;
    bool tone = false;
    {
        SourceWorker worker(src, [](auto) {}, [&](auto samples) {
            std::lock_guard lock(mu);
            for (float sample : samples) tone = tone || std::abs(sample) > 0.001f;
            ready.notify_all();
        });
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(8), [&] { return tone; });
    }
    CHECK(tone);
}

LTEST_MAIN("source workers")
