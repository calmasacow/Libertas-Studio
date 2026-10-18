// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// Drives the real libertas-render binary. A test that called composite_canvas in-process would
// pass with the process boundary broken, which is the boundary this preview exists to prove.

#include "lcomp/preview.hpp"
#include "ltest.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <sys/wait.h>
#include <thread>
#include <tuple>

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
        CHECK_EQ(pixel(first, 0, 0), std::tuple(255, 0, 0, 255));

        // The document changed. The next frame has to be the new picture, not the one still
        // sitting in the memfd from the commit above.
        red.settings["color"] = ldoc::Rgba::from_srgb8(0, 0, 255);
        project.sources = {red};
        const lcomp::PreviewFrame second = commit(link, project);
        CHECK_EQ(second.status, lcomp::PictureStatus::Complete);
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

        // Closing is the peer going away. Quit is the polite path; this is the one a crashed
        // client actually takes, and the renderer has to exit rather than wait.
        link.close();
        status = reap(*pid, 2000);
    }
    CHECK_EQ(status, 0);
    CHECK_EQ(count_fds(), fds_before);
}

LTEST_MAIN("preview")
