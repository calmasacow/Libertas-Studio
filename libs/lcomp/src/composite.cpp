// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "lcomp/composite.hpp"

#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace lcomp {

std::optional<int> quarter_turn(double degrees) {
    if (!std::isfinite(degrees))
        return std::nullopt;
    double wrapped = std::fmod(degrees, 360.0);
    if (wrapped < 0.0)
        wrapped += 360.0;
    const double steps = std::round(wrapped / 90.0);
    if (std::abs(wrapped - steps * 90.0) > 1e-4)
        return std::nullopt;
    int quarter = static_cast<int>(steps) % 4;
    if (quarter < 0)
        quarter += 4;
    return quarter;
}

bool transform_supported(const ldoc::Transform& t) {
    if (t.skew_deg.x != 0.0 || t.skew_deg.y != 0.0)
        return false;
    if (!quarter_turn(t.rotation_deg))
        return false;
    const double fields[] = {t.position.x, t.position.y,   t.scale.x,    t.scale.y,   t.pivot.x,
                             t.pivot.y,    t.rotation_deg, t.skew_deg.x, t.skew_deg.y};
    return std::ranges::all_of(fields, [](double v) { return std::isfinite(v); });
}

namespace {

/// A group's own transform is not applied to its children. The old rule stays for it: a flip
/// or a turn on the group would otherwise draw the children as if it were not there.
bool group_transform_ok(const ldoc::Transform& t) {
    if (t.rotation_deg != 0.0 || t.skew_deg.x != 0.0 || t.skew_deg.y != 0.0)
        return false;
    if (t.scale.x < 0.0 || t.scale.y < 0.0)
        return false;
    const double fields[] = {t.position.x, t.position.y,   t.scale.x,    t.scale.y,   t.pivot.x,
                             t.pivot.y,    t.rotation_deg, t.skew_deg.x, t.skew_deg.y};
    return std::ranges::all_of(fields, [](double v) { return std::isfinite(v); });
}

/// Clockwise, in screen space where Y grows downward. `(1, 0)` becomes `(0, 1)`.
void rotate_cw(int quarter, double x, double y, double& out_x, double& out_y) {
    switch (quarter) {
    case 1:
        out_x = -y;
        out_y = x;
        return;
    case 2:
        out_x = -x;
        out_y = -y;
        return;
    case 3:
        out_x = y;
        out_y = -x;
        return;
    default:
        out_x = x;
        out_y = y;
        return;
    }
}

/// The fitted content before scale and rotation. Scale and the quarter turn are applied by
/// the callers, so a resize can solve them without a second copy of the fit rules.
struct Unscaled {
    double base_w = 0;
    double base_h = 0;
    double rel_cx = 0;
    double rel_cy = 0;
    double rel_dx = 0;
    double rel_dy = 0;
};

std::optional<Unscaled> unscaled_content(const ldoc::Node& node, double natural_w, double natural_h,
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

    Unscaled out;
    out.base_w = dw;
    out.base_h = dh;
    out.rel_cx = bw * node.transform.pivot.x;
    out.rel_cy = bh * node.transform.pivot.y;
    out.rel_dx = dx - bx;
    out.rel_dy = dy - by;
    return out;
}

void scaled_corners(const Unscaled& u, double sx, double sy, double& left, double& top,
                    double& right, double& bottom) {
    left = (u.rel_dx - u.rel_cx) * sx;
    top = (u.rel_dy - u.rel_cy) * sy;
    right = left + u.base_w * sx;
    bottom = top + u.base_h * sy;
}

} // namespace

std::optional<DrawnFrame> drawn_frame(const ldoc::Node& node, double natural_w, double natural_h,
                                      bool have_natural, GapReason& why) {
    const std::optional<Unscaled> fitted =
        unscaled_content(node, natural_w, natural_h, have_natural, why);
    if (!fitted)
        return std::nullopt;

    const int quarter = quarter_turn(node.transform.rotation_deg).value_or(0);
    const double sx = node.transform.scale.x;
    const double sy = node.transform.scale.y;
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    scaled_corners(*fitted, sx, sy, left, top, right, bottom);

    const double xs[4] = {left, right, left, right};
    const double ys[4] = {top, top, bottom, bottom};
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    double max_x = -min_x;
    double max_y = -min_x;
    for (int i = 0; i < 4; ++i) {
        double rx = 0;
        double ry = 0;
        rotate_cw(quarter, xs[i], ys[i], rx, ry);
        min_x = std::min(min_x, rx);
        min_y = std::min(min_y, ry);
        max_x = std::max(max_x, rx);
        max_y = std::max(max_y, ry);
    }

    DrawnFrame frame;
    frame.base_width = fitted->base_w;
    frame.base_height = fitted->base_h;
    frame.rel_cx = fitted->rel_cx;
    frame.rel_cy = fitted->rel_cy;
    frame.rel_dx = fitted->rel_dx;
    frame.rel_dy = fitted->rel_dy;
    frame.x = node.transform.position.x + fitted->rel_cx + min_x;
    frame.y = node.transform.position.y + fitted->rel_cy + min_y;
    frame.width = max_x - min_x;
    frame.height = max_y - min_y;
    if (!(frame.width > 0.0) || !(frame.height > 0.0))
        return frame;

    constexpr double kBound = 1.0e7;
    if (!std::isfinite(frame.x) || !std::isfinite(frame.y) || !std::isfinite(frame.width) ||
        !std::isfinite(frame.height) || std::abs(frame.x) > kBound || std::abs(frame.y) > kBound ||
        frame.width > kBound || frame.height > kBound) {
        why = GapReason::UnsupportedTransform;
        return std::nullopt;
    }
    return frame;
}

std::optional<SolvedFrame> solve_drawn_rect(const ldoc::Node& node, double natural_w,
                                            double natural_h, bool have_natural, double x, double y,
                                            double width, double height) {
    GapReason why = GapReason::NoSize;
    const std::optional<Unscaled> fitted =
        unscaled_content(node, natural_w, natural_h, have_natural, why);
    if (!fitted || !(fitted->base_w > 0.0) || !(fitted->base_h > 0.0))
        return std::nullopt;
    const std::optional<int> quarter = quarter_turn(node.transform.rotation_deg);
    if (!quarter || !(width > 0.0) || !(height > 0.0) || !std::isfinite(x) || !std::isfinite(y))
        return std::nullopt;

    const double sign_x = node.transform.scale.x < 0.0 ? -1.0 : 1.0;
    const double sign_y = node.transform.scale.y < 0.0 ? -1.0 : 1.0;
    const bool turned = *quarter == 1 || *quarter == 3;
    const double abs_sx = turned ? height / fitted->base_w : width / fitted->base_w;
    const double abs_sy = turned ? width / fitted->base_h : height / fitted->base_h;
    if (!(abs_sx > 0.0) || !(abs_sy > 0.0) || !std::isfinite(abs_sx) || !std::isfinite(abs_sy))
        return std::nullopt;
    const double sx = sign_x * abs_sx;
    const double sy = sign_y * abs_sy;

    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    scaled_corners(*fitted, sx, sy, left, top, right, bottom);
    const double xs[4] = {left, right, left, right};
    const double ys[4] = {top, top, bottom, bottom};
    double min_x = std::numeric_limits<double>::infinity();
    double min_y = min_x;
    for (int i = 0; i < 4; ++i) {
        double rx = 0;
        double ry = 0;
        rotate_cw(*quarter, xs[i], ys[i], rx, ry);
        min_x = std::min(min_x, rx);
        min_y = std::min(min_y, ry);
    }

    SolvedFrame solved;
    solved.x = x - fitted->rel_cx - min_x;
    solved.y = y - fitted->rel_cy - min_y;
    solved.scale_x = sx;
    solved.scale_y = sy;
    if (!std::isfinite(solved.x) || !std::isfinite(solved.y))
        return std::nullopt;
    return solved;
}

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
    // Integer edges of `drawn_frame`. The overlay reads the continuous rectangle; the picture
    // reads these, and they have to be the same placement or the handles miss the pixels.
    const std::optional<DrawnFrame> frame =
        drawn_frame(node, natural_w, natural_h, have_natural, why);
    if (!frame)
        return std::nullopt;
    if (!(frame->width > 0.0) || !(frame->height > 0.0))
        return Px{};

    Px out;
    out.x0 = static_cast<int>(std::floor(frame->x));
    out.y0 = static_cast<int>(std::floor(frame->y));
    out.x1 = static_cast<int>(std::ceil(frame->x + frame->width));
    out.y1 = static_cast<int>(std::ceil(frame->y + frame->height));
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

/// One axis of the stretched blit. `flip` reads the far end of the destination first, which
/// is how a negative scale mirrors. The straight path uses this same division.
int sample_axis(int i, int dest_n, int src_n, bool flip) {
    if (dest_n <= 0 || src_n <= 0)
        return 0;
    const int from = flip ? dest_n - 1 - i : i;
    const int sampled = static_cast<int>((static_cast<std::int64_t>(from) * src_n) / dest_n);
    return std::clamp(sampled, 0, src_n - 1);
}

/// Copy an already-sRGB buffer into `picture`. A precomp's offscreen result. Opaque pixels are
/// copied; transparent ones leave the destination, which is why the offscreen buffer starts
/// clear rather than black — an empty nested scene must not punch a hole.
///
/// `quarter` is a clockwise quarter turn of the content inside `rect`, and the flips are a
/// negative scale applied before that turn. A solid colour does not come through here: its
/// bounds are enough.
void blit(Picture& picture, const Px& rect, const Picture& src, double opacity, int quarter,
          bool flip_x, bool flip_y) {
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
    const bool plain = quarter == 0 && !flip_x && !flip_y;

    for (int y = y0; y < y1; ++y) {
        const int iy = y - rect.y0;
        std::uint8_t* drow = picture.pixels.data() + static_cast<std::size_t>(y) * picture.stride;
        for (int x = x0; x < x1; ++x) {
            const int ix = x - rect.x0;
            int sx = 0;
            int sy = 0;
            if (plain) {
                sx = sample_axis(ix, dw, src.width, false);
                sy = sample_axis(iy, dh, src.height, false);
            } else {
                switch (quarter) {
                case 1:
                    sx = sample_axis(iy, dh, src.width, false);
                    sy = sample_axis(ix, dw, src.height, true);
                    break;
                case 2:
                    sx = sample_axis(ix, dw, src.width, true);
                    sy = sample_axis(iy, dh, src.height, true);
                    break;
                case 3:
                    sx = sample_axis(iy, dh, src.width, true);
                    sy = sample_axis(ix, dw, src.height, false);
                    break;
                default:
                    sx = sample_axis(ix, dw, src.width, false);
                    sy = sample_axis(iy, dh, src.height, false);
                    break;
                }
                if (flip_x)
                    sx = src.width - 1 - sx;
                if (flip_y)
                    sy = src.height - 1 - sy;
            }
            const std::uint8_t* srow =
                src.pixels.data() + static_cast<std::size_t>(sy) * src.stride;
            const std::uint8_t* s = srow + static_cast<std::size_t>(sx) * 4u;
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

/// The file named by the image source, at its decoded size. A missing or unreadable file is a
/// gap. Nothing is kept between pictures, so the next composite reads the file again.
void paint_image(const ldoc::Source& src, const ldoc::Node& node, double opacity,
                 CanvasTarget& target) {
    const auto it = src.settings.find("file");
    const auto* path = it == src.settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    if (path == nullptr || path->empty()) {
        refuse(target.picture, node, GapReason::Unreadable);
        return;
    }
    std::optional<DecodedImage> image = decode_image(*path);
    if (!image) {
        refuse(target.picture, node, GapReason::Unreadable);
        return;
    }
    GapReason why = GapReason::NoSize;
    const std::optional<Px> rect = place(node, static_cast<double>(image->width),
                                         static_cast<double>(image->height), true, why);
    if (!rect) {
        refuse(target.picture, node, why);
        return;
    }
    Picture sampled;
    sampled.width = image->width;
    sampled.height = image->height;
    sampled.stride = static_cast<std::uint32_t>(image->width) * 4u;
    sampled.pixels = std::move(image->pixels);
    target.drew = true;
    // One blend, the same one a colour uses. `alphaLinear` would be a second blend.
    blit(target.picture, *rect, sampled, opacity,
         quarter_turn(node.transform.rotation_deg).value_or(0), node.transform.scale.x < 0.0,
         node.transform.scale.y < 0.0);
}

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
    // A group does not push its transform onto its children. A flip or a turn on the group
    // itself would draw them as if it were not there, so the group keeps the stricter rule.
    const bool transform_ok = std::holds_alternative<ldoc::GroupData>(node.data)
                                  ? group_transform_ok(node.transform)
                                  : transform_supported(node.transform);
    if (!transform_ok) {
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
        if (src->type == "source.image") {
            paint_image(*src, node, opacity, target);
            return;
        }
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
        blit(target.picture, *rect, offscreen, opacity,
             quarter_turn(node.transform.rotation_deg).value_or(0), node.transform.scale.x < 0.0,
             node.transform.scale.y < 0.0);
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
