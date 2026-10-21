// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real libertas-render binary. A test that called composite_canvas in-process would
// pass with the process boundary broken, which is the boundary this preview exists to prove.

#include "lcomp/preview.hpp"
#include "ltest.hpp"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <lcapture/audio.hpp>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <utility>

#ifndef LIBERTAS_RENDER
#error "LIBERTAS_RENDER must name the libertas-render binary"
#endif

namespace {

int count_fds() {
    int n = 0;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++n;
    }
    return n;
}

/// Wait for the child to exit. A hang is a failed test, not a leaked process: kill it and say so.
int reap(pid_t pid, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        const pid_t got = ::waitpid(pid, &status, WNOHANG);
        if (got == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (got < 0)
            return -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

ldoc::Source colour_source(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Colour";
    source.type = "source.color";
    source.settings["color"] = ldoc::Rgba::from_srgb8(r, g, b);
    source.settings["width"] = static_cast<std::int64_t>(4);
    source.settings["height"] = static_cast<std::int64_t>(4);
    return source;
}

ldoc::Node bound_to(const ldoc::Source& source) {
    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, source.name);
    std::get<ldoc::SourceData>(node.data).source = source.id;
    return node;
}

ldoc::Project show(ldoc::Scene scene, std::vector<ldoc::Source> sources) {
    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = 4;
    project.canvases[0].height = 4;
    scene.canvas = project.canvases[0].id;
    project.decks[0].program_scene = scene.id;
    project.scenes = {std::move(scene)};
    project.sources = std::move(sources);
    return project;
}

std::tuple<int, int, int, int> pixel(const lcomp::PreviewFrame& frame, int x, int y) {
    const std::size_t i =
        static_cast<std::size_t>(y) * frame.stride + static_cast<std::size_t>(x) * 4u;
    return {frame.pixels[i], frame.pixels[i + 1], frame.pixels[i + 2], frame.pixels[i + 3]};
}

/// One commit, and the one frame it produced. Fails the case rather than returning a blank.
lcomp::PreviewFrame commit(lcomp::PreviewLink& link, const ldoc::Project& project) {
    const auto sent = link.send_project(project);
    REQUIRE_MSG(sent.has_value(), "{}", sent.error());
    lcomp::PreviewFrame frame;
    bool have = false;
    for (int i = 0; i < 8; ++i) {
        const auto event = link.receive(2000);
        REQUIRE_MSG(event.has_value(), "{}", event.error());
        REQUIRE_MSG(event->has_value(), "the renderer went quiet");
        const lcomp::PreviewEvent& got = **event;
        if (got.kind == lcomp::PreviewEvent::Kind::Fault)
            REQUIRE_MSG(false, "{}", got.fault);
        if (got.kind == lcomp::PreviewEvent::Kind::PeerGone)
            REQUIRE_MSG(false, "the renderer exited during a commit");
        if (got.kind == lcomp::PreviewEvent::Kind::Frame) {
            frame = got.frame;
            have = true;
        }
        if (got.kind == lcomp::PreviewEvent::Kind::Ack) {
            REQUIRE(have);
            return frame;
        }
    }
    REQUIRE_MSG(false, "the renderer never acknowledged the commit");
    return frame;
}

} // namespace

LTEST_CASE(render_lag_counts_composite_budgets_not_delayed_acknowledgments) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    bool server_ok = false;
    std::jthread server([channel = std::move(ends->second), &server_ok]() mutable {
        server_ok = lcomp::serve_preview(std::move(channel), {}, [] {
            // Delayed release/transport must not become another missed render frame.
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }, {}, [](const ldoc::Project& project, const ldoc::Id& canvas, std::uint32_t) {
            if (project.canvas(canvas)->name == "Slow")
                std::this_thread::sleep_for(std::chrono::milliseconds(45));
            return lcomp::composite_canvas(project, canvas);
        }).has_value();
    });
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto project = ldoc::make_default_project("render lag check");
    auto& canvas = project.canvases.front();
    canvas.width = canvas.height = 4;
    canvas.name = "Slow";
    canvas.fps = {1, 1};
    const auto first = commit(link, project);
    CHECK(first.render_ns >= 45000000u);
    CHECK_EQ(first.render_lag_frames, 0u);

    canvas.fps = {60000, 1001};
    const auto second = commit(link, project);
    const auto elapsed = second.render_ns - first.render_ns;
    const auto missed = static_cast<std::uint64_t>(
        std::ceil(static_cast<long double>(elapsed) * 60000 / 1001000000000.0L)) - 1;
    CHECK(missed >= 2u);
    CHECK_EQ(second.render_lag_frames, missed);
    CHECK_EQ(second.rendered_frames, 2u);

    // A separate canvas does not inherit another canvas's overruns.
    auto independent = ldoc::make_default_project("independent canvas");
    independent.canvases.front().width = independent.canvases.front().height = 4;
    independent.canvases.front().fps = {1, 1};
    const auto third = commit(link, independent);
    CHECK_EQ(third.rendered_frames, 1u);
    CHECK_EQ(third.render_lag_frames, 0u);
    link.quit();
    link.close();
    server.join();
    CHECK(server_ok);
}

LTEST_CASE(a_picture_crosses_the_process_and_the_renderer_exits_when_the_client_closes) {
    const int fds_before = count_fds();
    int status = -1;
    {
        auto ends = lipc::Channel::pair();
        REQUIRE_MSG(ends.has_value(), "{}", ends.error());
        auto pid = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
        REQUIRE_MSG(pid.has_value(), "{}", pid.error());

        lcomp::PreviewLink link{std::move(ends->first)};
        const auto hello = link.take_hello(2000);
        REQUIRE_MSG(hello.has_value(), "{}", hello.error());

        ldoc::Source red = colour_source(255, 0, 0);
        ldoc::Scene scene;
        scene.id = ldoc::Id::generate();
        scene.name = "Scene";
        scene.roots = {bound_to(red)};
        ldoc::Project project = show(scene, {red});

        const lcomp::PreviewFrame first = commit(link, project);
        CHECK_EQ(first.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(first.gaps, 0u);
        CHECK_EQ(first.rendered_frames, 1u);
        CHECK(first.render_ns > 0);
        CHECK_EQ(first.fps_num, 60u);
        CHECK_EQ(first.fps_den, 1u);
        CHECK_EQ(pixel(first, 0, 0), std::tuple(255, 0, 0, 255));

        // The document changed. The next frame has to be the new picture, not the one still
        // sitting in the memfd from the commit above.
        red.settings["color"] = ldoc::Rgba::from_srgb8(0, 0, 255);
        project.sources = {red};
        const lcomp::PreviewFrame second = commit(link, project);
        CHECK_EQ(second.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(second.rendered_frames, 2u);
        CHECK(second.render_ns >= first.render_ns);
        CHECK_EQ(pixel(second, 1, 1), std::tuple(0, 0, 255, 255));

        // A camera is not drawn. The colour underneath still is, and the gap count is what the
        // panel will say. Roots are topmost first, so the camera is in front.
        ldoc::Source camera;
        camera.id = ldoc::Id::generate();
        camera.name = "Cam";
        camera.type = "source.v4l2";
        ldoc::Node cam_node = bound_to(camera);
        scene.roots.insert(scene.roots.begin(), std::move(cam_node));
        project.scenes = {scene};
        project.sources = {red, camera};
        const lcomp::PreviewFrame gapped = commit(link, project);
        CHECK_EQ(gapped.status, lcomp::PictureStatus::Partial);
        CHECK_EQ(gapped.gaps, 1u);
        CHECK_EQ(pixel(gapped, 2, 2), std::tuple(0, 0, 255, 255));

        // A refresh uses the assembled document and a new size budget. Read the actual memfd
        // through the real process, including its resize, instead of testing a sizing helper.
        REQUIRE(link.refresh(2).has_value());
        auto meters = link.receive(2000);
        REQUIRE(meters && meters->has_value());
        CHECK_EQ((**meters).kind, lcomp::PreviewEvent::Kind::Audio);
        auto resized = link.receive(2000);
        REQUIRE(resized && resized->has_value());
        REQUIRE((**resized).kind == lcomp::PreviewEvent::Kind::Frame);
        CHECK_EQ((**resized).frame.width, 2);
        CHECK_EQ((**resized).frame.height, 2);
        CHECK_EQ(pixel((**resized).frame, 1, 1), std::tuple(0, 0, 255, 255));
        auto ack = link.receive(2000);
        REQUIRE(ack && ack->has_value());
        CHECK_EQ((**ack).kind, lcomp::PreviewEvent::Kind::Ack);
        REQUIRE(link.refresh(2).has_value());
        meters = link.receive(2000);
        REQUIRE(meters && meters->has_value());
        CHECK_EQ((**meters).kind, lcomp::PreviewEvent::Kind::Audio);
        auto unchanged = link.receive(2000);
        REQUIRE(unchanged && unchanged->has_value());
        CHECK_EQ((**unchanged).kind, lcomp::PreviewEvent::Kind::Ack);

        // A transparent precomp over a blue source must preserve the background and blend
        // its red child once. Both selected backends execute this through the process.
        ldoc::Source overlay = colour_source(255, 0, 0);
        ldoc::Scene inner;
        inner.id = ldoc::Id::generate();
        inner.canvas = project.canvases[0].id;
        auto child = bound_to(overlay);
        child.layout.size = {2, 2};
        child.layout.fit = ldoc::FitMode::Stretch;
        inner.roots = {child};
        auto nested = ldoc::make_node(ldoc::NodeKind::Precomp, "nested");
        std::get<ldoc::PrecompData>(nested.data).scene = inner.id;
        nested.blend.opacity = 0.5;
        scene.roots = {nested, bound_to(red)};
        project.sources = {red, overlay};
        project.scenes = {scene, inner};
        const auto blended = commit(link, project);
        CHECK_EQ(blended.status, lcomp::PictureStatus::Complete);
        CHECK_EQ(pixel(blended, 0, 0), std::tuple(188, 0, 188, 255));
        CHECK_EQ(pixel(blended, 3, 3), std::tuple(0, 0, 255, 255));

        // Closing is the peer going away. Quit is the polite path; this is the one a crashed
        // client actually takes, and the renderer has to exit rather than wait.
        link.close();
        status = reap(*pid, 2000);
    }
    CHECK_EQ(status, 0);
    CHECK_EQ(count_fds(), fds_before);
}

LTEST_CASE(text_source_crosses_worker_and_renderer_and_obeys_placement) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(1, 1, 1);
    source.type = "source.text";
    source.settings["text"] = std::string("Hi");
    source.settings["size"] = std::int64_t{20};
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{32};
    auto node = bound_to(source);
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {32, 32};
    node.transform.position = {32, 0};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.roots = {node};
    auto project = show(scene, {source});
    project.canvases[0].width = 64;
    project.canvases[0].height = 32;
    bool ink = false;
    for (int attempt = 0; attempt < 100 && !ink; ++attempt) {
        const auto frame = commit(link, project);
        if (frame.status == lcomp::PictureStatus::Complete) {
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 64; ++x) {
                    if (x < 32) CHECK_EQ(pixel(frame, x, y), std::make_tuple(0, 0, 0, 255));
                    else ink = ink || std::get<0>(pixel(frame, x, y)) > 0;
                }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 2000), 0);
    CHECK(ink);
}

LTEST_CASE(media_audio_crosses_worker_mixer_and_meter_transport) {
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion-tone.mkv";
    source.settings["width"] = std::int64_t{32};
    source.settings["height"] = std::int64_t{24};
    auto node = bound_to(source);
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    std::get<ldoc::SourceData>(node.data).audio.bus = project.buses.front().id;
    project.scenes[0].roots = {node};
    bool source_signal = false, bus_signal = false;
    REQUIRE(link.send_project(project).has_value());
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < until && !(source_signal && bus_signal)) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio) {
                if (level.node == node.id && level.available && level.left > 0.01f) source_signal = true;
                if (level.node == project.buses.front().id && level.available && level.left > 0.01f) bus_signal = true;
            }
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    project.buses.front().mute = true;
    bool muted = false;
    // Drain the in-flight refresh before sending an edited project.
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    REQUIRE(link.send_project(project).has_value());
    const auto mute_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < mute_until && !muted) {
        auto event = link.receive(1000);
        REQUIRE(event && event->has_value());
        if ((**event).kind == lcomp::PreviewEvent::Kind::Audio)
            for (const auto& level : (**event).audio)
                if (level.node == project.buses.front().id && level.available && level.left == 0) muted = true;
        if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            REQUIRE(link.refresh(64).has_value());
        }
    }
    for (int i = 0; i < 8; ++i) {
        auto event = link.receive(1000);
        if (event && event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) break;
    }
    link.quit(); link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    CHECK(source_signal);
    CHECK(bus_signal);
    CHECK(muted);
}

LTEST_CASE(monitoring_routes_real_renderer_audio_to_selected_sink_and_none) {
    if (!std::getenv("LIBERTAS_TEST_MONITOR"))
        return;
    std::string error;
    const auto base = "libertas_monitor_test_" + ldoc::Id::generate().to_string();
    auto sink1 = libertas::capture::AudioSink::open(base + "_1", error);
    auto sink2 = libertas::capture::AudioSink::open(base + "_2", error);
    REQUIRE_MSG(sink1 && sink2, "{}", error);

    struct Signal {
        std::mutex mutex;
        float peak = 0;

        void add(std::span<const float> samples) {
            std::lock_guard lock(mutex);
            for (const auto sample : samples)
                peak = std::max(peak, std::abs(sample));
        }

        float take() {
            std::lock_guard lock(mutex);
            return std::exchange(peak, 0);
        }
    } first, second;

    auto capture1 = libertas::capture::AudioCapture::open(
        base + "_1.monitor", true, [&](auto samples) { first.add(samples); }, error);
    auto capture2 = libertas::capture::AudioCapture::open(
        base + "_2.monitor", true, [&](auto samples) { second.add(samples); }, error);
    REQUIRE_MSG(capture1 && capture2, "{}", error);
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.media";
    source.settings["localFile"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/motion-tone.mkv";
    source.settings["loop"] = true;
    auto node = bound_to(source);
    std::get<ldoc::SourceData>(node.data).audio.monitor = true;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    project.scenes[0].roots = {node};
    for (const int selected : {-1, 1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10}) {
        project.sources.front().effects.clear();
        project.scenes.front().effects.clear();
        if (selected >= 3) {
            ldoc::Effect effect;
            effect.id = ldoc::Id::generate();
            effect.type = selected >= 7 ? "filter.gain" : "unknown.audio-filter";
            if (selected >= 7)
                effect.params["db"] = -60.0;
            effect.enabled = selected % 2 == 1;
            auto& stack = selected <= 4 || selected == 7 || selected == 8
                              ? project.sources.front().effects
                              : project.scenes.front().effects;
            stack.push_back(effect);
        }
        if (selected < 0)
            project.settings.extra.erase("audio.monitor.output");
        else
            project.settings.extra["audio.monitor.output"] =
                selected == 0 ? std::string("none")
                              : "sink:" + base + "_" + std::to_string(selected >= 3 ? 1 : selected);
        REQUIRE(link.send_project(project));
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(1400);
        bool pending = true, cleared = false;
        while (std::chrono::steady_clock::now() < end) {
            if (!cleared &&
                std::chrono::steady_clock::now() - start > std::chrono::milliseconds(500)) {
                first.take();
                second.take();
                cleared = true;
            }
            auto event = link.receive(50);
            REQUIRE(event);
            if (event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack) {
                pending = false;
                if (std::chrono::steady_clock::now() + std::chrono::milliseconds(100) < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    REQUIRE(link.refresh(64));
                    pending = true;
                }
            }
        }
        for (int i = 0; pending && i < 20; ++i) {
            auto event = link.receive(100);
            REQUIRE(event);
            if (event->has_value() && (**event).kind == lcomp::PreviewEvent::Kind::Ack)
                pending = false;
        }
        REQUIRE(!pending);
        CHECK_EQ(first.take() > 0.01f, selected == 1 || selected == 4 || selected == 6 ||
                                           selected == 8 || selected == 10);
        CHECK_EQ(second.take() > 0.01f, selected == 2);
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
    CHECK(!libertas::capture::AudioMonitor::open(error, base + "_missing"));
}

LTEST_CASE(video_capture_audio_uses_one_source_and_respects_none_and_deletion) {
    if (!std::getenv("LIBERTAS_TEST_MONITOR"))
        return;
    std::string error;
    const auto name = "libertas_video_audio_" + ldoc::Id::generate().to_string();
    auto input = libertas::capture::AudioSink::open(name, error);
    auto other_input = libertas::capture::AudioSink::open(name + "_other", error);
    auto output = libertas::capture::AudioSink::open(name + "_out", error);
    REQUIRE_MSG(input && other_input && output, "{}", error);
    auto tone = libertas::capture::AudioMonitor::open(error, name);
    auto other_tone = libertas::capture::AudioMonitor::open(error, name + "_other");
    REQUIRE_MSG(tone && other_tone, "{}", error);
    std::jthread feed([&](std::stop_token stop) {
        std::vector<float> block(960, 0.1f);
        std::vector<float> other_block(960, 0.2f);
        while (!stop.stop_requested()) {
            tone->write(block);
            other_tone->write(other_block);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    std::mutex mutex;
    float peak = 0;
    auto received = libertas::capture::AudioCapture::open(
        name + "_out.monitor", true,
        [&](auto samples) {
            std::lock_guard lock(mutex);
            for (float sample : samples)
                peak = std::max(peak, std::abs(sample));
        },
        error);
    REQUIRE(received);
    auto ends = lipc::Channel::pair();
    REQUIRE(ends);
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child);
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000));
    auto source = colour_source(0, 0, 0);
    source.type = "source.v4l2";
    source.settings["device"] = std::string("/dev/libertas-missing-test-video");
    source.settings["audioDevice"] = name + ".monitor";
    auto node = bound_to(source);
    std::get<ldoc::SourceData>(node.data).audio.monitor = true;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    std::get<ldoc::SourceData>(node.data).audio.bus = project.buses[0].id;
    project.settings.extra["audio.monitor.output"] = "sink:" + name + "_out";
    for (int step = 0; step < 9; ++step) {
        project.sources = {source};
        project.scenes[0].roots = {node};
        if (step == 1)
            project.sources[0].settings["audioDevice"] = std::string("none");
        if (step == 2)
            project.sources[0].settings["audioDevice"] = name + "_missing";
        if (step == 3)
            project.sources[0].settings["audioDevice"] = std::string("default");
        if (step == 4)
            project.scenes[0].roots[0].visible = false;
        if (step == 6) {
            project.sources.clear();
            project.scenes[0].roots.clear();
        }
        if (step == 7)
            project.sources[0].settings.erase("audioDevice");
        if (step == 8)
            project.sources[0].settings["audioDevice"] = name + "_other.monitor";
        REQUIRE(link.send_project(project));
        const auto start = std::chrono::steady_clock::now();
        const auto end = start + std::chrono::milliseconds(1400);
        bool pending = true, cleared = false, metered = false;
        while (std::chrono::steady_clock::now() < end) {
            if (!cleared &&
                std::chrono::steady_clock::now() - start > std::chrono::milliseconds(600)) {
                std::lock_guard lock(mutex);
                peak = 0;
                metered = false;
                cleared = true;
            }
            auto event = link.receive(50);
            REQUIRE(event);
            if (!event->has_value())
                continue;
            if ((**event).kind == lcomp::PreviewEvent::Kind::Audio && cleared)
                for (const auto& level : (**event).audio)
                    if (level.node == node.id && level.available && level.left > 0.01f)
                        metered = true;
            if ((**event).kind == lcomp::PreviewEvent::Kind::Ack) {
                pending = false;
                if (std::chrono::steady_clock::now() + std::chrono::milliseconds(100) < end) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    REQUIRE(link.refresh(64));
                    pending = true;
                }
            }
        }
        REQUIRE(!pending);
        const bool audible = step == 0 || step == 5 || step == 8;
        CHECK_EQ(metered, audible);
        std::lock_guard lock(mutex);
        CHECK_EQ(peak > 0.01f, audible);
        if (step == 8) CHECK(peak > 0.15f);
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 3000), 0);
}

LTEST_CASE(video_audio_default_matches_physical_capture_card) {
    const char* video = std::getenv("LIBERTAS_TEST_VIDEO_AUDIO");
    if (!video)
        return;
    std::string error;
    const auto input = libertas::capture::video_audio_device(video, error);
    REQUIRE_MSG(!input.empty(), "{}", error);
    const char* expected = std::getenv("LIBERTAS_TEST_VIDEO_AUDIO_EXPECTED");
    REQUIRE(expected);
    CHECK_EQ(input, std::string(expected));
}

LTEST_CASE(capture_reuses_one_snapshot_across_direct_and_nested_placements) {
    const char* connector = std::getenv("LIBERTAS_TEST_CAPTURE");
    const char* video = std::getenv("LIBERTAS_TEST_VIDEO");
    const char* window = std::getenv("LIBERTAS_TEST_WINDOW");
    if (!connector && !video && !window)
        return;
    auto ends = lipc::Channel::pair();
    REQUIRE(ends.has_value());
    auto child = lcomp::spawn_renderer(LIBERTAS_RENDER, std::move(ends->second).take_socket());
    REQUIRE(child.has_value());
    lcomp::PreviewLink link{std::move(ends->first)};
    REQUIRE(link.take_hello(2000).has_value());
    auto source = colour_source(0, 0, 0);
    source.type = window ? "source.window" : video ? "source.v4l2" : "source.screen";
    if (window) source.settings["window"] = std::string(window);
    else if (video) {
        source.settings["device"] = std::string(video);
        source.settings["resolution"] = std::string("2560x1440");
        source.settings["frameRate"] = std::string("60");
    } else source.settings["output"] = std::string(connector);
    auto node = bound_to(source);
    node.layout.size = {64, 72};
    node.layout.fit = ldoc::FitMode::Stretch;
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    auto project = show(scene, {source});
    project.canvases[0].width = 128;
    project.canvases[0].height = 72;
    ldoc::Scene inner;
    inner.id = ldoc::Id::generate();
    inner.canvas = project.canvases[0].id;
    auto duplicate = node;
    duplicate.id = ldoc::Id::generate();
    inner.roots = {node, duplicate};
    auto nested = ldoc::make_node(ldoc::NodeKind::Precomp, "same screen again");
    auto& pre = std::get<ldoc::PrecompData>(nested.data);
    pre.scene = inner.id;
    pre.size = {64, 72};
    nested.transform.position = {64, 0};
    project.scenes[0].roots = {node, nested};
    project.scenes.push_back(inner);
    if (video && !window) {
        auto hidden = project;
        hidden.decks.front().program_scene.reset();
        commit(link, hidden);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        CHECK(commit(link, project).status == lcomp::PictureStatus::Complete);
    }
    int complete = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (complete < 20 && std::chrono::steady_clock::now() < deadline) {
        const auto frame = commit(link, project);
        if (frame.status == lcomp::PictureStatus::Complete) {
            ++complete;
            for (int y = 0; y < 72; ++y)
                for (int x = 0; x < 64; ++x)
                    CHECK_EQ(pixel(frame, x, y), pixel(frame, x + 64, y));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (video && !window) {
        const auto device = std::filesystem::canonical(video);
        const auto device_open = [&] {
            std::error_code ec;
            for (const auto& fd : std::filesystem::directory_iterator(
                     "/proc/" + std::to_string(*child) + "/fd", ec)) {
                if (std::filesystem::canonical(fd.path(), ec) == device && !ec) return true;
                ec.clear();
            }
            return false;
        };
        const auto active = project.scenes.front().id;
        ldoc::Scene blank;
        blank.id = ldoc::Id::generate();
        blank.canvas = project.canvases.front().id;
        project.scenes.push_back(blank);
        CHECK(device_open());
        for (int i = 0; i < 3; ++i) {
            project.decks.front().program_scene = blank.id;
            commit(link, project);
            CHECK(device_open());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            project.decks.front().program_scene = active;
            CHECK(commit(link, project).status == lcomp::PictureStatus::Complete);
        }
        // The setting itself must not restart a visible device.
        project.sources.front().settings["deactivateWhenHidden"] = true;
        CHECK(commit(link, project).status == lcomp::PictureStatus::Complete);
        project.decks.front().program_scene = blank.id;
        commit(link, project);
        CHECK(!device_open());
        project.sources.front().settings["deactivateWhenHidden"] = false;
        project.decks.front().program_scene = active;
        commit(link, project);
        CHECK(device_open());
        project.decks.front().program_scene = blank.id;
        commit(link, project);
        CHECK(device_open());
        project.sources.clear();
        commit(link, project);
        CHECK(!device_open());
    }
    link.quit();
    link.close();
    CHECK_EQ(reap(*child, 2000), 0);
    CHECK_EQ(complete, 20);
}

int main() {
    if (!std::getenv("LIBERTAS_TEST_CAPTURE") && !std::getenv("LIBERTAS_TEST_VIDEO") && !std::getenv("LIBERTAS_TEST_WINDOW"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name == "capture_reuses_one_snapshot_across_direct_and_nested_placements";
        });
    return ltest::run_all("preview");
}
