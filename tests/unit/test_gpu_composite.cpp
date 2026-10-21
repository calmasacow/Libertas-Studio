// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Explicit hardware check: LIBERTAS_TEST_GPU=1. No fallback is allowed to pass this check.
#include "ltest.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <lcapture/screen.hpp>
#include <lrhi/composite.hpp>
#include <mutex>

namespace {
ldoc::Project show(int width = 48, int height = 32) {
    auto p = ldoc::make_default_project("GPU check");
    p.canvases[0].width = width;
    p.canvases[0].height = height;
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.type = "source.screen";
    auto node = ldoc::make_node(ldoc::NodeKind::Source, "screen");
    std::get<ldoc::SourceData>(node.data).source = source.id;
    node.layout.fit = ldoc::FitMode::Stretch;
    node.layout.size = {static_cast<double>(width), static_cast<double>(height)};
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.canvas = p.canvases[0].id;
    scene.roots = {node};
    p.sources = {source};
    p.scenes = {scene};
    p.decks[0].program_scene = scene.id;
    return p;
}

void compare(const lcomp::Picture& cpu, const lcomp::Picture& gpu) {
    REQUIRE(cpu.width == gpu.width);
    REQUIRE(cpu.height == gpu.height);
    REQUIRE(cpu.status == gpu.status);
    REQUIRE(cpu.gaps.size() == gpu.gaps.size());
    REQUIRE(cpu.pixels.size() == gpu.pixels.size());
    int maximum = 0;
    for (std::size_t i = 0; i < cpu.pixels.size(); ++i)
        maximum = std::max(maximum, std::abs(int(cpu.pixels[i]) - int(gpu.pixels[i])));
    CHECK_MSG(maximum <= 2, "GPU/CPU maximum channel error: {} (allowed 2)", maximum);
}

libertas::rhi::VkCtxPtr device() {
    std::string error;
    const char* selected = std::getenv("LIBERTAS_RENDER_DEVICE");
    auto result = libertas::rhi::vk_init(selected ? selected : "", error);
    REQUIRE_MSG(result != nullptr, "{}", error);
    return result;
}
} // namespace

LTEST_CASE(gpu_executes_the_shared_walk_for_opacity_precomps_rotations_and_flips) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    std::vector<std::uint8_t> pixels(7 * 5 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<std::uint8_t>((i * 37 + 11) % 256);
    lcomp::FrameSource frames = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{7, 5, pixels.data(), pixels.size()};
    };
    auto project = show();
    auto node = project.scenes[0].roots[0];
    node.layout.size = {17, 13};
    node.transform.position = {-2, 3};
    node.layout.crop = {1, 1, 2, 1};
    ldoc::Scene inner = project.scenes[0];
    inner.id = ldoc::Id::generate();
    inner.roots[0].blend.opacity = 0.7;
    auto precomp = ldoc::make_node(ldoc::NodeKind::Precomp, "nested screen");
    std::get<ldoc::PrecompData>(precomp.data).scene = inner.id;
    precomp.blend.opacity = 0.6;
    project.scenes.push_back(inner);
    for (int quarter = 0; quarter < 4; ++quarter) {
        for (int flips = 0; flips < 4; ++flips) {
            node.transform.rotation_deg = quarter * 90;
            node.transform.scale = {flips & 1 ? -1.0 : 1.0, flips & 2 ? -1.0 : 1.0};
            node.transform.pivot = {0.5, 0.5};
            node.blend.opacity = 0.53;
            project.scenes[0].roots = {node, precomp};
            const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, frames);
            REQUIRE(plan.gaps.empty());
            for (std::uint32_t edge : {0u, 19u}) {
                auto result = gpu->render(plan, edge, error);
                REQUIRE_MSG(result.has_value(), "{}", error);
                compare(lcomp::preview_size(lcomp::execute_cpu(plan), edge), *result);
            }
        }
    }
    // The other texture path is the real decoder, with RGBA instead of capture's BGRA.
    project.sources[0].type = "source.image";
    project.scenes[0].roots[0].layout.crop = {};
    project.sources[0].settings["file"] = std::string(LIBERTAS_IMAGE_FIXTURES) + "/two.png";
    auto group = ldoc::make_node(ldoc::NodeKind::Group, "opacity group");
    group.blend.opacity = 0.5;
    group.transform.position = {2, 1};
    group.transform.scale = {0.5, 0.75};
    group.transform.rotation_deg = 90;
    std::get<ldoc::GroupData>(group.data).children = project.scenes[0].roots;
    project.scenes[0].roots = {group};
    const auto image_plan = lcomp::plan_canvas(project, project.canvases[0].id);
    auto image_result = gpu->render(image_plan, 0, error);
    REQUIRE_MSG(image_result.has_value(), "{}", error);
    compare(lcomp::execute_cpu(image_plan), *image_result);
    std::get<ldoc::GroupData>(project.scenes[0].roots[0].data).size = ldoc::Vec2{8, 6};
    const auto bounded_plan = lcomp::plan_canvas(project, project.canvases[0].id);
    auto bounded_result = gpu->render(bounded_plan, 0, error);
    REQUIRE_MSG(bounded_result.has_value(), "{}", error);
    compare(lcomp::execute_cpu(bounded_plan), *bounded_result);
}

LTEST_CASE(all_seven_blends_match_cpu_for_textures_transparency_and_nested_groups) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto project = show();
    std::vector<std::uint8_t> pixels(7 * 5 * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<std::uint8_t>(50 + (i * 37) % 180);
    const lcomp::FrameSource frames = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{7, 5, pixels.data(), pixels.size()};
    };
    auto front = project.scenes[0].roots[0];
    auto background = front;
    background.blend.opacity = 0.7;
    auto inner = project.scenes[0];
    inner.id = ldoc::Id::generate();
    auto precomp = ldoc::make_node(ldoc::NodeKind::Precomp, "transparent nested scene");
    std::get<ldoc::PrecompData>(precomp.data).scene = inner.id;
    project.scenes.push_back(inner);
    using M = ldoc::BlendMode;
    for (const auto mode :
         {M::Normal, M::LinearDodge, M::Subtract, M::Screen, M::Multiply, M::Lighten, M::Darken}) {
        for (const double opacity : {0.0, 0.53, 1.0}) {
            front.blend.mode = mode;
            front.blend.opacity = opacity;
            front.transform.rotation_deg = 90;
            front.transform.scale = {-1, 1};
            project.scenes[1].roots = {front, background};
            auto group = ldoc::make_node(ldoc::NodeKind::Group, "blended group");
            group.blend.mode = mode;
            group.blend.opacity = opacity;
            group.transform.position = {2, 1};
            group.transform.scale = {0.8, 0.7};
            std::get<ldoc::GroupData>(group.data).children = {precomp, background};
            project.scenes[0].roots = {group, background};
            const auto plan = lcomp::plan_canvas(project, project.canvases[0].id, frames);
            REQUIRE(plan.gaps.empty());
            for (const auto edge : {0u, 19u}) {
                auto result = gpu->render(plan, edge, error);
                REQUIRE_MSG(result, "{}", error);
                compare(lcomp::preview_size(lcomp::execute_cpu(plan), edge), *result);
            }
        }
    }
}

LTEST_CASE(host_video_uploads_to_a_4k_canvas) {
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto project = show(3840, 2160);
    std::vector<std::uint8_t> pixels(2560 * 1440 * 4, 255);
    const auto plan = lcomp::plan_canvas(project, project.canvases[0].id,
        [&](const ldoc::Source&) {
            return lcomp::CapturedPicture{2560, 1440, pixels.data(), pixels.size()};
        });
    REQUIRE(gpu->render(plan, 640, error).has_value());
    const auto started = std::chrono::steady_clock::now();
    const int count = std::getenv("LIBERTAS_BENCHMARK") ? 180 : 3;
    for (int i = 0; i < count; ++i) {
        auto frame = gpu->render(plan, 640, error);
        REQUIRE_MSG(frame.has_value(), "{}", error);
        CHECK_EQ(frame->width, 640);
        CHECK_EQ(frame->height, 360);
        CHECK_EQ(frame->pixels.front(), 255);
    }
    if (std::getenv("LIBERTAS_BENCHMARK"))
        std::fprintf(stderr, "1440p host upload + 4K composite + 640 preview: %.3f ms/frame\n",
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / count);
}

LTEST_CASE(a_real_capture_slot_stays_on_device_until_preview_readback) {
    const char* output = std::getenv("LIBERTAS_TEST_CAPTURE");
    if (!output)
        return;
    auto d = device();
    std::string error;
    auto gpu = libertas::rhi::GpuCompositor::create(*d, error);
    REQUIRE_MSG(gpu != nullptr, "{}", error);
    auto capture = libertas::capture::ScreenCapture::open(*d, output, true, error);
    REQUIRE_MSG(capture != nullptr, "{}", error);
    std::mutex mu;
    std::condition_variable ready;
    std::shared_ptr<const libertas::capture::DmabufImage> frame;
    capture->start_images([&](auto image) {
        std::lock_guard lock(mu);
        frame = std::move(image);
        ready.notify_one();
    });
    std::shared_ptr<const libertas::capture::DmabufImage> snapshot;
    {
        std::unique_lock lock(mu);
        ready.wait_for(lock, std::chrono::seconds(5), [&] { return bool(frame); });
        snapshot = frame;
    }
    // Stop before any assertion can unwind the callback's mutex and condition variable.
    capture->stop();
    REQUIRE_MSG(snapshot != nullptr, "no first capture frame within five seconds");
    auto p = show(static_cast<int>(snapshot->width), static_cast<int>(snapshot->height));
    lcomp::FrameSource native = [&](const ldoc::Source&) {
        lcomp::CapturedPicture result;
        result.width = static_cast<int>(snapshot->width);
        result.height = static_cast<int>(snapshot->height);
        result.device_image = snapshot.get();
        return result;
    };
    const auto plan = lcomp::plan_canvas(p, p.canvases[0].id, native);
    std::optional<lcomp::Picture> result;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
        result = gpu->render(plan, 640, error);
        REQUIRE_MSG(result.has_value(), "{}", error);
    }
    std::fprintf(stderr,
                 "capture %ux%u, GPU composite + 640-edge preview: %.3f ms/frame (60 frames)\n",
                 snapshot->width, snapshot->height,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                         .count() /
                     60);
    std::vector<std::uint8_t> host;
    REQUIRE_MSG(libertas::rhi::vk_readback_exported(*d, *snapshot, true, host, error), "{}", error);
    lcomp::FrameSource cpu = [&](const ldoc::Source&) {
        return lcomp::CapturedPicture{static_cast<int>(snapshot->width),
                                      static_cast<int>(snapshot->height), host.data(), host.size()};
    };
    compare(lcomp::preview_size(lcomp::composite_canvas(p, p.canvases[0].id, cpu), 640), *result);
    auto invalid = *snapshot;
    invalid.dmabuf_fd = -1;
    const auto invalid_plan = lcomp::plan_canvas(p, p.canvases[0].id, [&](const ldoc::Source&) {
        auto frame_view = native(p.sources.front());
        frame_view.device_image = &invalid;
        return frame_view;
    });
    CHECK(!gpu->render(invalid_plan, 640, error).has_value());
    CHECK(error.find("producer fence") != std::string::npos);
    REQUIRE_MSG(gpu->render(plan, 640, error).has_value(), "{}", error);
}

int main() {
    if (!std::getenv("LIBERTAS_TEST_GPU"))
        return 77;
    if (!std::getenv("LIBERTAS_TEST_CAPTURE"))
        std::erase_if(ltest::registry(), [](const auto& test) {
            return test.name == "a_real_capture_slot_stays_on_device_until_preview_readback";
        });
    return ltest::run_all("GPU composite");
}
