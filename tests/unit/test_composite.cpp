// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/composite.hpp"
#include "ltest.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

using lcomp::GapReason;
using lcomp::Picture;
using lcomp::PictureStatus;

namespace {

ldoc::Source colour_source(std::uint8_t r, std::uint8_t g, std::uint8_t b, int w, int h) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Colour";
    source.type = "source.color";
    source.settings["color"] = ldoc::Rgba::from_srgb8(r, g, b);
    source.settings["width"] = static_cast<std::int64_t>(w);
    source.settings["height"] = static_cast<std::int64_t>(h);
    return source;
}

ldoc::Node bound_to(const ldoc::Source& source) {
    ldoc::Node node = ldoc::make_node(ldoc::NodeKind::Source, source.name);
    std::get<ldoc::SourceData>(node.data).source = source.id;
    return node;
}

/// A project whose one canvas is `edge` pixels square and whose program scene is `scene`.
ldoc::Project show(int edge, ldoc::Scene scene, std::vector<ldoc::Source> sources = {}) {
    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = edge;
    project.canvases[0].height = edge;
    scene.canvas = project.canvases[0].id;
    project.decks[0].program_scene = scene.id;
    project.scenes = {std::move(scene)};
    project.sources = std::move(sources);
    return project;
}

ldoc::Scene scene_named(std::string name) {
    ldoc::Scene scene;
    scene.id = ldoc::Id::generate();
    scene.name = std::move(name);
    return scene;
}

ldoc::Source image_source(std::string path) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Still";
    source.type = "source.image";
    source.settings["file"] = std::move(path);
    return source;
}

std::filesystem::path image_fixture(const char* name) {
    return std::filesystem::path{LIBERTAS_IMAGE_FIXTURES} / name;
}

std::tuple<int, int, int, int> pixel(const Picture& picture, int x, int y) {
    const std::size_t i =
        static_cast<std::size_t>(y) * picture.stride + static_cast<std::size_t>(x) * 4u;
    return {picture.pixels[i], picture.pixels[i + 1], picture.pixels[i + 2], picture.pixels[i + 3]};
}

} // namespace

LTEST_CASE(red_covers_the_canvas_and_black_is_what_nothing_draws) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(red)};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);

    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(255, 0, 0, 255));

    project.scenes[0].roots.clear();
    const Picture empty = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(empty.status, PictureStatus::Empty);
    CHECK_EQ(pixel(empty, 1, 1), std::tuple(0, 0, 0, 255));

    project.decks[0].program_scene.reset();
    const Picture none = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(none.status, PictureStatus::NoProgram);
    CHECK_EQ(pixel(none, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(the_top_of_the_list_is_drawn_last) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    red.name = "Red";
    ldoc::Source blue = colour_source(0, 0, 255, 4, 4);
    blue.name = "Blue";
    ldoc::Scene scene = scene_named("Scene");
    // roots[0] is the topmost node. Red in front of blue is red.
    scene.roots = {bound_to(red), bound_to(blue)};
    ldoc::Project project = show(4, scene, {red, blue});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 2, 2), std::tuple(255, 0, 0, 255));
}

LTEST_CASE(a_hidden_node_is_not_a_gap_and_is_not_drawn) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node node = bound_to(red);
    node.visible = false;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Empty);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(half_opacity_blends_in_linear_light) {
    // 50% opaque red over the black backdrop. Linear 0.5 encodes to sRGB 188.
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node node = bound_to(red);
    node.blend.opacity = 0.5;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 1, 2), std::tuple(188, 0, 0, 255));
}

LTEST_CASE(a_groups_opacity_reaches_its_children) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Node child = bound_to(red);
    ldoc::Node group = ldoc::make_node(ldoc::NodeKind::Group, "Group");
    group.blend.opacity = 0.5;
    std::get<ldoc::GroupData>(group.data).children = {child};
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {group};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(188, 0, 0, 255));

    project.scenes[0].roots[0].visible = false;
    const Picture hidden = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(hidden.status, PictureStatus::Empty);
    CHECK_EQ(pixel(hidden, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(contain_letterboxes_a_colour) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 2);
    ldoc::Node node = bound_to(red);
    node.layout.fit = ldoc::FitMode::Contain;
    node.layout.size = {4.0, 4.0};
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {node};
    ldoc::Project project = show(4, scene, {red});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(picture, 0, 1), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 2), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(what_cannot_be_drawn_is_a_gap_and_the_rest_still_is) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Source camera;
    camera.id = ldoc::Id::generate();
    camera.name = "Desk Cam";
    camera.type = "source.v4l2";
    ldoc::Node cam = bound_to(camera);
    ldoc::Node card = bound_to(red);
    // The camera is on top. It contributes nothing, so the colour behind it is the picture.
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {cam, card};

    ldoc::Node unbound = ldoc::make_node(ldoc::NodeKind::Source, "Loose");
    scene.roots.insert(scene.roots.begin(), unbound);

    ldoc::Node turned = bound_to(red);
    turned.name = "Turned";
    turned.transform.rotation_deg = 15.0;
    scene.roots.push_back(turned);

    ldoc::Source mic;
    mic.id = ldoc::Id::generate();
    mic.name = "Mic";
    mic.type = "source.audio-input";
    scene.roots.push_back(bound_to(mic));

    ldoc::Project project = show(4, scene, {red, camera, mic});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));

    int cameras = 0;
    int loose = 0;
    int turned_n = 0;
    for (const lcomp::Gap& gap : picture.gaps) {
        if (gap.reason == GapReason::UnsupportedType && gap.name == "Desk Cam")
            ++cameras;
        if (gap.reason == GapReason::NoSource)
            ++loose;
        if (gap.reason == GapReason::UnsupportedTransform)
            ++turned_n;
    }
    CHECK_EQ(cameras, 1);
    CHECK_EQ(loose, 1);
    CHECK_EQ(turned_n, 1);
    // The microphone is not a picture, so it is not a failure to draw one.
    CHECK_EQ(picture.gaps.size(), 3u);
}

LTEST_CASE(a_precomp_draws_the_other_scene_and_a_cycle_stops) {
    ldoc::Source blue = colour_source(0, 0, 255, 4, 4);
    blue.name = "Blue";
    ldoc::Scene inner = scene_named("Inner");
    inner.roots = {bound_to(blue)};

    ldoc::Node pre = ldoc::make_node(ldoc::NodeKind::Precomp, "Inner");
    std::get<ldoc::PrecompData>(pre.data).scene = inner.id;
    std::get<ldoc::PrecompData>(pre.data).size = {4.0, 4.0};
    ldoc::Scene outer = scene_named("Outer");
    outer.roots = {pre};

    ldoc::Project project = ldoc::make_default_project("show");
    project.canvases[0].width = 4;
    project.canvases[0].height = 4;
    inner.canvas = project.canvases[0].id;
    outer.canvas = project.canvases[0].id;
    project.decks[0].program_scene = outer.id;
    project.scenes = {outer, inner};
    project.sources = {blue};

    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 1, 1), std::tuple(0, 0, 255, 255));

    // Point the inner scene back at the outer one. Drawing has to finish, and say why.
    ldoc::Node back = ldoc::make_node(ldoc::NodeKind::Precomp, "Back");
    std::get<ldoc::PrecompData>(back.data).scene = outer.id;
    project.scenes[1].roots.push_back(back);
    const Picture cycled = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(cycled.status, PictureStatus::Partial);
    bool saw_cycle = false;
    for (const lcomp::Gap& gap : cycled.gaps)
        if (gap.reason == GapReason::Cycle)
            saw_cycle = true;
    CHECK(saw_cycle);
    CHECK_EQ(pixel(cycled, 0, 0), std::tuple(0, 0, 255, 255));
}

LTEST_CASE(a_colour_with_no_colour_stored_is_not_invented) {
    ldoc::Source source;
    source.id = ldoc::Id::generate();
    source.name = "Bare";
    source.type = "source.color";
    source.settings["width"] = static_cast<std::int64_t>(4);
    source.settings["height"] = static_cast<std::int64_t>(4);
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(source)};
    ldoc::Project project = show(4, scene, {source});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::NoColour);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_png_is_drawn_at_its_own_size_and_a_clear_pixel_stays_black) {
    ldoc::Source still = image_source(image_fixture("two.png").string());
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(still)};
    ldoc::Project project = show(4, scene, {still});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Complete);
    CHECK(picture.gaps.empty());
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(picture, 1, 0), std::tuple(0, 255, 0, 255));
    CHECK_EQ(pixel(picture, 0, 1), std::tuple(0, 0, 255, 255));
    CHECK_EQ(pixel(picture, 1, 1), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(picture, 3, 3), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_jpeg_and_a_webp_are_drawn) {
    ldoc::Source jpeg = image_source(image_fixture("red.jpg").string());
    jpeg.name = "Jpeg";
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(jpeg)};
    ldoc::Project project = show(2, scene, {jpeg});
    const Picture jpg = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(jpg.status, PictureStatus::Complete);
    // cjpeg -quality 100 of solid red, decoded by the same libjpeg, is 254.
    CHECK_EQ(pixel(jpg, 0, 0), std::tuple(254, 0, 0, 255));
    CHECK_EQ(pixel(jpg, 1, 1), std::tuple(254, 0, 0, 255));

    ldoc::Source webp = image_source(image_fixture("red.webp").string());
    project.sources = {webp};
    project.scenes[0].roots = {bound_to(webp)};
    const Picture web = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(web.status, PictureStatus::Complete);
    CHECK_EQ(pixel(web, 0, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(web, 1, 1), std::tuple(255, 0, 0, 255));
}

LTEST_CASE(an_image_that_cannot_be_read_leaves_what_is_behind_it) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 4);
    ldoc::Source missing = image_source("/no/such/libertas-still.png");
    missing.name = "Missing";
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {bound_to(missing), bound_to(red)};
    ldoc::Project project = show(4, scene, {red, missing});
    const Picture picture = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(picture.status, PictureStatus::Partial);
    CHECK_EQ(pixel(picture, 0, 0), std::tuple(255, 0, 0, 255));
    REQUIRE(!picture.gaps.empty());
    CHECK_EQ(picture.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(picture.gaps.front().name, "Missing");

    ldoc::Source blank = image_source("");
    project.sources = {blank};
    project.scenes[0].roots = {bound_to(blank)};
    const Picture empty = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(empty.status, PictureStatus::Partial);
    CHECK_EQ(empty.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(pixel(empty, 0, 0), std::tuple(0, 0, 0, 255));

    const auto junk = std::filesystem::temp_directory_path() / "libertas-not-an-image";
    {
        std::ofstream out(junk);
        out << "not a picture\n";
    }
    ldoc::Source garbage = image_source(junk.string());
    project.sources = {garbage};
    project.scenes[0].roots = {bound_to(garbage)};
    const Picture refused = lcomp::composite_canvas(project, project.canvases[0].id);
    std::filesystem::remove(junk);
    CHECK_EQ(refused.gaps.front().reason, GapReason::Unreadable);

    ldoc::Source huge = image_source(image_fixture("huge.png").string());
    project.sources = {huge};
    project.scenes[0].roots = {bound_to(huge)};
    const Picture oversized = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(oversized.gaps.front().reason, GapReason::Unreadable);
    CHECK_EQ(pixel(oversized, 0, 0), std::tuple(0, 0, 0, 255));
}

LTEST_CASE(a_quarter_turn_and_a_flip_are_drawn_and_a_slant_is_not) {
    ldoc::Source red = colour_source(255, 0, 0, 4, 2);
    ldoc::Node card = bound_to(red);
    card.transform.position = {2.0, 3.0};
    card.transform.rotation_deg = 90.0;
    ldoc::Scene scene = scene_named("Scene");
    scene.roots = {card};
    ldoc::Project project = show(8, scene, {red});
    const Picture turned = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(turned.status, PictureStatus::Complete);
    CHECK(turned.gaps.empty());
    // 4 by 2 at (2, 3), pivot in the middle, turned clockwise: a 2 by 4 at (3, 2).
    CHECK_EQ(pixel(turned, 3, 2), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(turned, 4, 5), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(turned, 2, 3), std::tuple(0, 0, 0, 255));
    CHECK_EQ(pixel(turned, 5, 2), std::tuple(0, 0, 0, 255));

    ldoc::Source still = image_source(image_fixture("two.png").string());
    ldoc::Node photo = bound_to(still);
    photo.transform.rotation_deg = 90.0;
    project.sources = {still};
    project.scenes[0].roots = {photo};
    const Picture spun = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(spun.status, PictureStatus::Complete);
    // two.png is red, green / blue, clear. Clockwise, that is blue, red / clear, green.
    CHECK_EQ(pixel(spun, 0, 0), std::tuple(0, 0, 255, 255));
    CHECK_EQ(pixel(spun, 1, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(spun, 1, 1), std::tuple(0, 255, 0, 255));

    photo.transform.rotation_deg = 0.0;
    photo.transform.scale.x = -1.0;
    project.scenes[0].roots = {photo};
    const Picture flipped = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(flipped.status, PictureStatus::Complete);
    CHECK_EQ(pixel(flipped, 0, 0), std::tuple(0, 255, 0, 255));
    CHECK_EQ(pixel(flipped, 1, 0), std::tuple(255, 0, 0, 255));
    CHECK_EQ(pixel(flipped, 1, 1), std::tuple(0, 0, 255, 255));

    photo.transform.scale = {1.0, 1.0};
    photo.transform.rotation_deg = 15.0;
    project.scenes[0].roots = {photo};
    const Picture slanted = lcomp::composite_canvas(project, project.canvases[0].id);
    CHECK_EQ(slanted.status, PictureStatus::Partial);
    REQUIRE(!slanted.gaps.empty());
    CHECK_EQ(slanted.gaps.front().reason, GapReason::UnsupportedTransform);

    ldoc::Node plain = bound_to(red);
    GapReason why = GapReason::NoSize;
    const auto solved = lcomp::solve_drawn_rect(plain, 4.0, 2.0, true, 0.0, 0.0, 2.0, 1.0);
    REQUIRE(solved.has_value());
    CHECK(std::abs(solved->scale_x - 0.5) < 1e-9);
    CHECK(std::abs(solved->scale_y - 0.5) < 1e-9);
    plain.transform.position = {solved->x, solved->y};
    plain.transform.scale = {solved->scale_x, solved->scale_y};
    const auto frame = lcomp::drawn_frame(plain, 4.0, 2.0, true, why);
    REQUIRE(frame.has_value());
    CHECK(std::abs(frame->x) < 1e-6);
    CHECK(std::abs(frame->y) < 1e-6);
    CHECK(std::abs(frame->width - 2.0) < 1e-6);
    CHECK(std::abs(frame->height - 1.0) < 1e-6);
}

LTEST_MAIN("composite")
