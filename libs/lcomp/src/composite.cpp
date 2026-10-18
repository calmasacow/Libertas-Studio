// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/composite.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace lcomp {
namespace {

/// The inverse of the curve in `Rgba::to_srgb8`. Copied rather than shared because that function
/// is local to the document library and the two have to stay inverses: the half-coverage test
/// is a literal sRGB byte, and a drift here is what it would catch.
double srgb_to_linear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

bool audio_only(std::string_view type) {
    return type == "source.audio-input" || type == "source.audio-output";
}

bool effect_enabled(const ldoc::Node& node) {
    return std::ranges::any_of(node.effects, [](const ldoc::Effect& e) { return e.enabled; });
}

bool transform_supported(const ldoc::Transform& t) {
    if (t.rotation_deg != 0.0 || t.skew_deg.x != 0.0 || t.skew_deg.y != 0.0)
        return false;
    if (t.scale.x < 0.0 || t.scale.y < 0.0)
        return false;
    const double fields[] = {t.position.x, t.position.y,   t.scale.x,    t.scale.y,   t.pivot.x,
                             t.pivot.y,    t.rotation_deg, t.skew_deg.x, t.skew_deg.y};
    return std::ranges::all_of(fields, [](double v) { return std::isfinite(v); });
}

struct Px {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool empty() const { return x1 <= x0 || y1 <= y0; }
};

/// Where a node's box lands, in pixel edges, half-open.
///
/// Natural size is the source's own size after crop. The box is `layout.size` when it is set
/// and the natural size otherwise — the document's rule that a zero box means "the source's
/// natural size". Fit then places the source inside that box, and scale acts around the box's
/// pivot. Integer-aligned inputs stay on pixel edges; nothing here antialiases, so a test can
/// name a pixel and mean it.
std::optional<Px> place(const ldoc::Node& node, double natural_w, double natural_h,
                        bool have_natural, GapReason& why) {
    if (!transform_supported(node.transform)) {
        why = GapReason::UnsupportedTransform;
        return std::nullopt;
    }

    const bool have_box = node.layout.size.x > 0.0 && node.layout.size.y > 0.0;
    if (node.layout.fit == ldoc::FitMode::Natural && !have_natural && !have_box) {
        why = GapReason::NoSize;
        return std::nullopt;
    }
    if (!have_box && !have_natural) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    const double bx = node.transform.position.x;
    const double by = node.transform.position.y;
    const double bw = have_box ? node.layout.size.x : natural_w;
    const double bh = have_box ? node.layout.size.y : natural_h;
    if (!(bw > 0.0) || !(bh > 0.0) || !std::isfinite(bw) || !std::isfinite(bh)) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    double sw = have_natural ? natural_w : bw;
    double sh = have_natural ? natural_h : bh;
    const ldoc::CropRect& crop = node.layout.crop;
    sw -= crop.left + crop.right;
    sh -= crop.top + crop.bottom;
    if (!(sw > 0.0) || !(sh > 0.0) || !std::isfinite(sw) || !std::isfinite(sh)) {
        why = GapReason::NoSize;
        return std::nullopt;
    }

    double dx = bx;
    double dy = by;
    double dw = bw;
    double dh = bh;
    const double ax = node.layout.alignment.x;
    const double ay = node.layout.alignment.y;
    switch (node.layout.fit) {
    case ldoc::FitMode::Natural:
        dw = sw;
        dh = sh;
        dx = bx + crop.left;
        dy = by + crop.top;
        break;
    case ldoc::FitMode::Stretch:
        break;
    case ldoc::FitMode::Contain: {
        const double s = std::min(bw / sw, bh / sh);
        dw = sw * s;
        dh = sh * s;
        dx = bx + (bw - dw) * ax;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::Cover: {
        const double s = std::max(bw / sw, bh / sh);
        dw = sw * s;
        dh = sh * s;
        dx = bx + (bw - dw) * ax;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::FillWidth: {
        const double s = bw / sw;
        dw = bw;
        dh = sh * s;
        dx = bx;
        dy = by + (bh - dh) * ay;
        break;
    }
    case ldoc::FitMode::FillHeight: {
        const double s = bh / sh;
        dh = bh;
        dw = sw * s;
        dy = by;
        dx = bx + (bw - dw) * ax;
        break;
    }
    }

    const double sx = node.transform.scale.x;
    const double sy = node.transform.scale.y;
    if (sx != 1.0 || sy != 1.0) {
        const double cx = bx + bw * node.transform.pivot.x;
        const double cy = by + bh * node.transform.pivot.y;
        dx = cx + (dx - cx) * sx;
        dy = cy + (dy - cy) * sy;
        dw *= sx;
        dh *= sy;
    }
    if (!(dw > 0.0) || !(dh > 0.0))
        return Px{};

    // A position a corrupt document can carry, and which does not fit in an int. Refuse it
    // rather than letting the cast overflow.
    constexpr double kBound = 1.0e7;
    if (!std::isfinite(dx) || !std::isfinite(dy) || std::abs(dx) > kBound ||
        std::abs(dy) > kBound || dw > kBound || dh > kBound) {
        why = GapReason::UnsupportedTransform;
        return std::nullopt;
    }

    Px out;
    out.x0 = static_cast<int>(std::floor(dx));
    out.y0 = static_cast<int>(std::floor(dy));
    out.x1 = static_cast<int>(std::ceil(dx + dw));
    out.y1 = static_cast<int>(std::ceil(dy + dh));
    return out;
}

void refuse(Picture& picture, const ldoc::Node& node, GapReason why) {
    picture.gaps.push_back(Gap{node.id, node.name, why});
}

/// Straight linear source over the sRGB8 pixel already in the buffer.
///
/// `Rgba` is stored straight, not premultiplied: `from_srgb8` divides the alpha and converts
/// the channels independently. Premultiplying happens here, for the blend, and nowhere else.
void blend_linear(std::uint8_t* d, double sr, double sg, double sb, double sa) {
    if (sa >= 1.0) {
        const auto bytes = ldoc::Rgba{sr, sg, sb, 1.0}.to_srgb8();
        d[0] = bytes[0];
        d[1] = bytes[1];
        d[2] = bytes[2];
        d[3] = 255;
        return;
    }
    if (sa <= 0.0)
        return;

    const double dr = srgb_to_linear(static_cast<double>(d[0]) / 255.0);
    const double dg = srgb_to_linear(static_cast<double>(d[1]) / 255.0);
    const double db = srgb_to_linear(static_cast<double>(d[2]) / 255.0);
    const double da = static_cast<double>(d[3]) / 255.0;
    const double inv = 1.0 - sa;
    const double or_ = sr * sa + dr * da * inv;
    const double og = sg * sa + dg * da * inv;
    const double ob = sb * sa + db * da * inv;
    const double oa = sa + da * inv;
    if (oa <= 0.0) {
        d[0] = d[1] = d[2] = d[3] = 0;
        return;
    }
    const auto bytes = ldoc::Rgba{or_ / oa, og / oa, ob / oa, oa}.to_srgb8();
    d[0] = bytes[0];
    d[1] = bytes[1];
    d[2] = bytes[2];
    d[3] = bytes[3];
}

void fill_rect(Picture& picture, const Px& rect, const ldoc::Rgba& colour, double opacity) {
    const double sa = colour.a * std::clamp(opacity, 0.0, 1.0);
    if (sa <= 0.0 || rect.empty())
        return;
    const int x0 = std::max(0, rect.x0);
    const int y0 = std::max(0, rect.y0);
    const int x1 = std::min(picture.width, rect.x1);
    const int y1 = std::min(picture.height, rect.y1);
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* row = picture.pixels.data() + static_cast<std::size_t>(y) * picture.stride;
        for (int x = x0; x < x1; ++x)
            blend_linear(row + static_cast<std::size_t>(x) * 4u, colour.r, colour.g, colour.b, sa);
    }
}

/// Copy an already-sRGB buffer into `picture`. A precomp's offscreen result. Opaque pixels are
/// copied; transparent ones leave the destination, which is why the offscreen buffer starts
/// clear rather than black — an empty nested scene must not punch a hole.
void blit(Picture& picture, const Px& rect, const Picture& src, double opacity) {
    if (rect.empty() || src.width <= 0 || src.height <= 0 || opacity <= 0.0)
        return;
    const int x0 = std::max(0, rect.x0);
    const int y0 = std::max(0, rect.y0);
    const int x1 = std::min(picture.width, rect.x1);
    const int y1 = std::min(picture.height, rect.y1);
    const int dw = rect.x1 - rect.x0;
    const int dh = rect.y1 - rect.y0;
    if (dw <= 0 || dh <= 0)
        return;
    const double covered = std::clamp(opacity, 0.0, 1.0);

    for (int y = y0; y < y1; ++y) {
        const int sy = static_cast<int>((static_cast<std::int64_t>(y - rect.y0) * src.height) / dh);
        const int clamped_y = std::clamp(sy, 0, src.height - 1);
        const std::uint8_t* srow =
            src.pixels.data() + static_cast<std::size_t>(clamped_y) * src.stride;
        std::uint8_t* drow = picture.pixels.data() + static_cast<std::size_t>(y) * picture.stride;
        for (int x = x0; x < x1; ++x) {
            const int sx =
                static_cast<int>((static_cast<std::int64_t>(x - rect.x0) * src.width) / dw);
            const int clamped_x = std::clamp(sx, 0, src.width - 1);
            const std::uint8_t* s = srow + static_cast<std::size_t>(clamped_x) * 4u;
            std::uint8_t* d = drow + static_cast<std::size_t>(x) * 4u;
            if (s[3] == 0)
                continue;
            if (s[3] == 255 && covered >= 1.0) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
                continue;
            }
            const double sa = (static_cast<double>(s[3]) / 255.0) * covered;
            blend_linear(d, srgb_to_linear(static_cast<double>(s[0]) / 255.0),
                         srgb_to_linear(static_cast<double>(s[1]) / 255.0),
                         srgb_to_linear(static_cast<double>(s[2]) / 255.0), sa);
        }
    }
}

std::optional<double> setting_number(const ldoc::ParamMap& settings, const char* key) {
    const auto it = settings.find(key);
    if (it == settings.end())
        return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&it->second))
        return static_cast<double>(*i);
    if (const auto* n = std::get_if<double>(&it->second))
        return *n;
    return std::nullopt;
}

struct CanvasTarget {
    Picture& picture;
    bool& drew;
    /// Scenes currently being drawn. A precomp that names one of them is a cycle, not a loop.
    std::vector<ldoc::Id>& stack;
};

void paint_scene(const ldoc::Project& project, const ldoc::Scene& scene, double opacity,
                 CanvasTarget& target);

void paint_node(const ldoc::Project& project, const ldoc::Node& node, double parent_opacity,
                CanvasTarget& target) {
    if (!node.visible || parent_opacity <= 0.0)
        return;
    const double opacity = parent_opacity * std::clamp(node.blend.opacity, 0.0, 1.0);
    if (opacity <= 0.0)
        return;

    // A group in a mode other than Normal would need flattening before the blend, and a source
    // in one of those modes is a different picture than the one Normal would make. Drawing the
    // children anyway would be that different picture. One gap, on the node that asked.
    if (node.blend.mode != ldoc::BlendMode::Normal || node.blend.clip_to_below) {
        refuse(target.picture, node, GapReason::UnsupportedBlend);
        return;
    }
    if (effect_enabled(node)) {
        refuse(target.picture, node, GapReason::UnsupportedEffect);
        return;
    }
    if (!transform_supported(node.transform)) {
        refuse(target.picture, node, GapReason::UnsupportedTransform);
        return;
    }

    if (const auto* group = std::get_if<ldoc::GroupData>(&node.data)) {
        // Children are listed topmost first. Walk them back to front so the top is drawn last.
        for (auto it = group->children.rbegin(); it != group->children.rend(); ++it)
            paint_node(project, *it, opacity, target);
        return;
    }

    if (const auto* source = std::get_if<ldoc::SourceData>(&node.data)) {
        if (!source->source) {
            refuse(target.picture, node, GapReason::NoSource);
            return;
        }
        const ldoc::Source* src = project.source(*source->source);
        if (!src) {
            refuse(target.picture, node, GapReason::NoSource);
            return;
        }
        if (audio_only(src->type))
            return;
        if (src->type != "source.color") {
            refuse(target.picture, node, GapReason::UnsupportedType);
            return;
        }
        const auto colour_it = src->settings.find("color");
        const ldoc::Rgba* colour = colour_it == src->settings.end()
                                       ? nullptr
                                       : std::get_if<ldoc::Rgba>(&colour_it->second);
        if (!colour) {
            refuse(target.picture, node, GapReason::NoColour);
            return;
        }
        const auto width = setting_number(src->settings, "width");
        const auto height = setting_number(src->settings, "height");
        const bool have_natural = width && height && *width > 0.0 && *height > 0.0;
        GapReason why = GapReason::NoSize;
        const std::optional<Px> rect = place(node, have_natural ? *width : 0.0,
                                             have_natural ? *height : 0.0, have_natural, why);
        if (!rect) {
            refuse(target.picture, node, why);
            return;
        }
        target.drew = true;
        fill_rect(target.picture, *rect, *colour, opacity);
        return;
    }

    if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
        if (std::ranges::find(target.stack, pre->scene) != target.stack.end()) {
            refuse(target.picture, node, GapReason::Cycle);
            return;
        }
        const ldoc::Scene* inner = project.scene(pre->scene);
        if (!inner) {
            refuse(target.picture, node, GapReason::MissingScene);
            return;
        }
        // (0, 0) means the canvas this precomp is being drawn into. That is what PrecompData
        // says, and a second opinion here would be a second size.
        int pw = target.picture.width;
        int ph = target.picture.height;
        if (pre->size.x > 0.0 && pre->size.y > 0.0) {
            if (pre->size.x > kMaxPreviewEdge || pre->size.y > kMaxPreviewEdge) {
                refuse(target.picture, node, GapReason::NoSize);
                return;
            }
            pw = static_cast<int>(std::lround(pre->size.x));
            ph = static_cast<int>(std::lround(pre->size.y));
        }
        if (pw <= 0 || ph <= 0 || pw > kMaxPreviewEdge || ph > kMaxPreviewEdge) {
            refuse(target.picture, node, GapReason::NoSize);
            return;
        }
        Picture offscreen;
        offscreen.width = pw;
        offscreen.height = ph;
        offscreen.stride = static_cast<std::uint32_t>(pw) * 4u;
        const auto bytes =
            static_cast<std::uint64_t>(offscreen.stride) * static_cast<std::uint64_t>(ph);
        offscreen.pixels.assign(static_cast<std::size_t>(bytes), 0);

        // A local, not `target.drew`. The inner scene's emptiness is not the outer scene's, and
        // the reference cannot be reseated — assigning through it would wipe a node already
        // drawn behind this precomp.
        bool nested_drew = false;
        target.stack.push_back(inner->id);
        CanvasTarget nested{offscreen, nested_drew, target.stack};
        paint_scene(project, *inner, 1.0, nested);
        target.stack.pop_back();
        for (Gap& gap : offscreen.gaps)
            target.picture.gaps.push_back(std::move(gap));

        GapReason why = GapReason::NoSize;
        const std::optional<Px> rect =
            place(node, static_cast<double>(pw), static_cast<double>(ph), true, why);
        if (!rect) {
            refuse(target.picture, node, why);
            return;
        }
        target.drew = true;
        blit(target.picture, *rect, offscreen, opacity);
        return;
    }

    // Adjustment, mask, reference: each changes the picture in a way this composite does not
    // do. A reference in particular must not draw the target twice and call it the same.
    refuse(target.picture, node, GapReason::UnsupportedType);
}

void paint_scene(const ldoc::Project& project, const ldoc::Scene& scene, double opacity,
                 CanvasTarget& target) {
    for (auto it = scene.roots.rbegin(); it != scene.roots.rend(); ++it)
        paint_node(project, *it, opacity, target);
}

void fill_opaque_black(Picture& picture) {
    const std::size_t n = picture.pixels.size();
    for (std::size_t i = 3; i < n; i += 4)
        picture.pixels[i] = 255;
}

} // namespace

Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas_id) {
    Picture picture;
    const ldoc::Canvas* canvas = project.canvas(canvas_id);
    if (!canvas) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }
    picture.width = canvas->width;
    picture.height = canvas->height;
    if (canvas->width <= 0 || canvas->height <= 0 || canvas->width > kMaxPreviewEdge ||
        canvas->height > kMaxPreviewEdge) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }
    const auto bytes =
        static_cast<std::uint64_t>(canvas->width) * static_cast<std::uint64_t>(canvas->height) * 4u;
    constexpr std::uint64_t kMaxBytes = 128ull * 1024ull * 1024ull;
    if (bytes > kMaxBytes ||
        bytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        picture.status = PictureStatus::Unsupported;
        return picture;
    }
    picture.stride = static_cast<std::uint32_t>(canvas->width) * 4u;
    picture.pixels.assign(static_cast<std::size_t>(bytes), 0);
    fill_opaque_black(picture);

    const ldoc::Deck* deck = nullptr;
    for (const ldoc::Deck& candidate : project.decks) {
        if (candidate.canvas == canvas_id) {
            deck = &candidate;
            break;
        }
    }
    if (!deck || !deck->program_scene) {
        picture.status = PictureStatus::NoProgram;
        return picture;
    }
    const ldoc::Scene* scene = project.scene(*deck->program_scene);
    if (!scene) {
        picture.status = PictureStatus::NoProgram;
        return picture;
    }

    bool drew = false;
    std::vector<ldoc::Id> stack;
    stack.push_back(scene->id);
    CanvasTarget target{picture, drew, stack};
    paint_scene(project, *scene, 1.0, target);

    if (!picture.gaps.empty())
        picture.status = PictureStatus::Partial;
    else if (!drew)
        picture.status = PictureStatus::Empty;
    else
        picture.status = PictureStatus::Complete;
    return picture;
}

} // namespace lcomp
