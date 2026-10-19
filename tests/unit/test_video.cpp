// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <lcapture/video.hpp>
#include "ltest.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

LTEST_CASE(unavailable_video_is_an_error_not_an_invented_frame) {
    std::string error;
    CHECK(!libertas::capture::VideoCapture::open("/dev/video999999", "", "", "", error));
    CHECK(!error.empty());
}

LTEST_CASE(elgato_streams_1440p60_and_held_frames_are_immutable) {
    const char* device = std::getenv("LIBERTAS_TEST_VIDEO");
    if (!device) return;
    std::string error;
    auto capture = libertas::capture::VideoCapture::open(device, "2560x1440", "60", "Default", error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    std::mutex mu;
    std::condition_variable changed;
    int count = 0;
    std::shared_ptr<const libertas::capture::VideoFrame> held;
    std::vector<std::uint8_t> original;
    std::chrono::steady_clock::time_point first, last;
    capture->start([&](auto frame) {
        if (!frame) return;
        std::lock_guard lock(mu);
        if (!held) {
            held = frame;
            original = frame->pixels;
            first = std::chrono::steady_clock::now();
        }
        last = std::chrono::steady_clock::now();
        ++count;
        changed.notify_all();
    });
    {
        std::unique_lock lock(mu);
        changed.wait_for(lock, std::chrono::seconds(8), [&] { return count >= 180; });
    }
    capture->stop();
    REQUIRE(held != nullptr);
    CHECK_EQ(held->width, 2560);
    CHECK_EQ(held->height, 1440);
    CHECK_EQ(held->pixels.size(), std::size_t{2560 * 1440 * 4});
    CHECK(held->pixels == original);
    REQUIRE(count >= 180);
    const double fps = static_cast<double>(count - 1) / std::chrono::duration<double>(last - first).count();
    std::fprintf(stderr, "Elgato capture + BGRA conversion: %.2f fps (%d frames)\n", fps, count);
    CHECK(fps > 55.0);
}

LTEST_MAIN("video")
