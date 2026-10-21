// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "image.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <lcomp/composite.hpp>
#include <lcomp/filters.hpp>
#include <map>
#include <fstream>
#include <sstream>
#include <numbers>

namespace lcomp {
namespace {
FilterField number(std::string key, std::string label, double value, double lo, double hi) {
    return {std::move(key), std::move(label), "real", value, lo, hi};
}

FilterField integer(std::string key, std::string label, int value, int lo, int hi) {
    return {std::move(key),      std::move(label),        "int",
            std::int64_t{value}, static_cast<double>(lo), static_cast<double>(hi)};
}

const ldoc::ParamValue* value(const ldoc::Effect& effect, std::string_view key) {
    if (const auto it = effect.params.find(std::string(key)); it != effect.params.end())
        return &it->second;
    if (const auto* info = filter_info(effect.type))
        for (const auto& field : info->fields)
            if (field.key == key)
                return &field.value;
    return nullptr;
}

float clamp(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

std::shared_ptr<Picture> mask_image(const std::string& path) {
    struct Entry {
        std::filesystem::file_time_type time;
        std::uintmax_t bytes;
        std::shared_ptr<Picture> image;
    };

    static thread_local std::map<std::string, Entry> cache;
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec)
        return {};
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec)
        return {};
    if (auto it = cache.find(path);
        it != cache.end() && it->second.time == time && it->second.bytes == bytes)
        return it->second.image;
    auto decoded = decode_image(path);
    if (!decoded)
        return {};
    auto image = std::make_shared<Picture>();
    image->width = decoded->width;
    image->height = decoded->height;
    image->stride = static_cast<unsigned>(image->width) * 4;
    image->pixels = std::move(decoded->pixels);
    // ponytail: retain eight decoded masks; raise cache budget only after measuring mask-heavy
    // shows.
    if (cache.size() >= 8)
        cache.clear();
    cache[path] = {time, bytes, image};
    return image;
}
} // namespace

const std::vector<FilterInfo>& filter_catalog() {
    static const std::vector<FilterInfo> catalog{
        {"filter.color-correction",
         "Color Correction",
         "color_filter",
         false,
         {number("gamma", "Gamma", 0, -3, 3), number("contrast", "Contrast", 0, -2, 2),
          number("brightness", "Brightness", 0, -1, 1),
          number("saturation", "Saturation", 0, -1, 5),
          number("hue_shift", "Hue shift", 0, -180, 180), number("opacity", "Opacity", 1, 0, 1),
          integer("color_multiply", "Multiply color (RGB)", 0xffffff, 0, 0xffffff),
          integer("color_add", "Add color (RGB)", 0, 0, 0xffffff)}},
        {"filter.crop",
         "Crop / Pad",
         "crop_filter",
         false,
         {{"relative", "Relative", "bool", true},
          integer("left", "Left", 0, -8192, 8192),
          integer("top", "Top", 0, -8192, 8192),
          integer("right", "Right", 0, -8192, 8192),
          integer("bottom", "Bottom", 0, -8192, 8192),
          integer("cx", "Width", 1920, 1, 8192),
          integer("cy", "Height", 1080, 1, 8192)}},
        {"filter.scale",
         "Scaling / Aspect Ratio",
         "scale_filter",
         false,
         {integer("width", "Width", 1920, 1, 8192), integer("height", "Height", 1080, 1, 8192)}},
        {"filter.color-key",
         "Color Key",
         "color_key_filter",
         false,
         {integer("key_color", "Key color (RGB)", 0x00ff00, 0, 0xffffff),
          number("similarity", "Similarity", 0.08, 0, 1),
          number("smoothness", "Smoothness", 0.05, 0.001, 1)}},
        {"filter.chroma-key",
         "Chroma Key",
         "chroma_key_filter",
         false,
         {integer("key_color", "Key color (RGB)", 0x00ff00, 0, 0xffffff),
          number("similarity", "Similarity", 0.4, 0, 1),
          number("smoothness", "Smoothness", 0.08, 0.001, 1),
          number("spill", "Spill reduction", 0.1, 0.001, 1)}},
        {"filter.luma-key",
         "Luma Key",
         "luma_key_filter",
         false,
         {number("luma_min", "Minimum", 0, 0, 1), number("luma_max", "Maximum", 1, 0, 1),
          number("luma_min_smooth", "Minimum smoothness", 0, 0, 1),
          number("luma_max_smooth", "Maximum smoothness", 0, 0, 1)}},
        {"filter.sharpen",
         "Sharpen",
         "sharpness_filter",
         false,
         {number("sharpness", "Sharpness", 0.08, 0, 1)}},
        {"filter.mask",
         "Image Mask / Blend",
         "mask_filter",
         false,
         {{"path", "Image file", "text", std::string{}},
          {"mode",
           "Mode",
           "choice",
           std::string{"alpha-color"},
           0,
           0,
           {"alpha-color", "alpha", "multiply", "add", "subtract"}},
          number("opacity", "Opacity", 1, 0, 1)}},
        {"filter.gain", "Gain", "gain_filter", true, {number("db", "Gain (dB)", 0, -60, 60)}},
        {"filter.invert", "Invert Polarity", "invert_polarity_filter", true, {}},
        {"filter.equalizer",
         "3-Band Equalizer",
         "basic_eq_filter",
         true,
         {number("low", "Low (dB)", 0, -20, 20), number("mid", "Mid (dB)", 0, -20, 20),
          number("high", "High (dB)", 0, -20, 20)}},
        {"filter.noise-gate",
         "Noise Gate",
         "noise_gate_filter",
         true,
         {number("open_threshold", "Open threshold (dB)", -26, -96, 0),
          number("close_threshold", "Close threshold (dB)", -32, -96, 0),
          number("attack_time", "Attack (ms)", 25, 0, 10000),
          number("hold_time", "Hold (ms)", 200, 0, 10000),
          number("release_time", "Release (ms)", 150, 0, 10000)}},
        {"filter.compressor",
         "Compressor",
         "compressor_filter",
         true,
         {number("ratio", "Ratio", 10, 1, 100),
          number("threshold", "Threshold (dB)", -18, -96, 0),
          number("attack_time", "Attack (ms)", 6, 0, 10000),
          number("release_time", "Release (ms)", 60, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"sidechain", "Sidechain source ID (or none)", "text", std::string{"none"}}}},
        {"filter.limiter",
         "Limiter",
         "limiter_filter",
         true,
         {number("threshold", "Threshold (dB)", -6, -96, 0),
          number("release_time", "Release (ms)", 60, 0, 10000)}},
        {"filter.expander",
         "Expander",
         "expander_filter",
         true,
         {number("ratio", "Ratio", 2, 1, 20),
          number("threshold", "Threshold (dB)", -40, -96, 0),
          number("attack_time", "Attack (ms)", 10, 0, 10000),
          number("release_time", "Release (ms)", 50, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"detector", "Detection", "choice", std::string{"RMS"}, 0, 0, {"RMS", "peak"}}}},
        {"filter.upward-compressor",
         "Upward Compressor",
         "upward_compressor_filter",
         true,
         {number("ratio", "Ratio", 0.5, 0.01, 1),
          number("threshold", "Threshold (dB)", -20, -96, 0),
          number("attack_time", "Attack (ms)", 10, 0, 10000),
          number("release_time", "Release (ms)", 50, 0, 10000),
          number("output_gain", "Output gain (dB)", 0, -60, 60),
          {"detector", "Detection", "choice", std::string{"RMS"}, 0, 0, {"RMS", "peak"}},
          number("knee", "Knee (dB)", 10, 0, 20)}},
        {"filter.noise-suppression",
         "Noise Suppression",
         "noise_suppress_filter",
         true,
         {{"method", "Method", "choice", std::string{"rnnoise"}, 0, 0, {"rnnoise", "speex"}},
          number("suppress_level", "Suppression (dB)", -30, -60, 0)}}};
    return catalog;
}

const FilterInfo* filter_info(std::string_view type) {
    for (const auto& info : filter_catalog())
        if (info.type == type)
            return &info;
    return nullptr;
}

double filter_number(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    if (v) {
        if (const auto* d = std::get_if<double>(v))
            return *d;
        if (const auto* i = std::get_if<std::int64_t>(v))
            return static_cast<double>(*i);
    }
    return 0;
}

std::string filter_text(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    const auto* s = v ? std::get_if<std::string>(v) : nullptr;
    return s ? *s : std::string{};
}

bool filter_flag(const ldoc::Effect& effect, std::string_view key) {
    const auto* v = value(effect, key);
    const auto* b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}

std::expected<void, std::string> validate_filter(const ldoc::Effect& effect) {
    const auto* info = filter_info(effect.type);
    if (!info)
        return std::unexpected("Unknown filter type: " + effect.type);
    if (effect.mask || effect.blend)
        return std::unexpected("Filter mask/blend override is unsupported");
    for (const auto& field : info->fields) {
        const auto* v = value(effect, field.key);
        if (!v)
            return std::unexpected("Missing filter setting: " + field.key);
        if (field.kind == "real" || field.kind == "int") {
            if (!std::holds_alternative<double>(*v) && !std::holds_alternative<std::int64_t>(*v))
                return std::unexpected("Filter setting must be numeric: " + field.key);
            const auto n = filter_number(effect, field.key);
            if (!std::isfinite(n) || n < field.minimum || n > field.maximum ||
                (field.kind == "int" && n != std::floor(n)))
                return std::unexpected("Filter setting out of range: " + field.key);
        } else if (field.kind == "bool" && !std::holds_alternative<bool>(*v))
            return std::unexpected("Filter setting must be boolean: " + field.key);
        else if (field.kind != "bool" && !std::holds_alternative<std::string>(*v))
            return std::unexpected("Filter setting must be text: " + field.key);
        if (field.kind == "choice" &&
            std::ranges::find(field.choices, filter_text(effect, field.key)) == field.choices.end())
            return std::unexpected("Unknown filter choice: " + field.key);
    }
    return {};
}

bool video_filters_supported(std::span<const ldoc::Effect> effects) {
    return std::ranges::all_of(
        effects, [](const auto& e) { return !e.enabled || validate_filter(e).has_value(); });
}

std::expected<ldoc::Vec2, std::string> filter_size(std::span<const ldoc::Effect> effects, double w,
                                                   double h) {
    for (const auto& e : effects) {
        if (!e.enabled)
            continue;
        if (const auto* info = filter_info(e.type); info && info->audio)
            continue;
        if (auto valid = validate_filter(e); !valid)
            return std::unexpected(valid.error());
        if (e.type == "filter.mask" && !mask_image(filter_text(e, "path")))
            return std::unexpected("Mask image is missing or unreadable");
        if (e.type == "filter.crop") {
            w = filter_flag(e, "relative")
                    ? w - filter_number(e, "left") - filter_number(e, "right")
                    : filter_number(e, "cx");
            h = filter_flag(e, "relative")
                    ? h - filter_number(e, "top") - filter_number(e, "bottom")
                    : filter_number(e, "cy");
        } else if (e.type == "filter.scale") {
            w = filter_number(e, "width");
            h = filter_number(e, "height");
        }
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > 8192 || h > 8192 ||
            w * h > 33554432)
            return std::unexpected("Filtered image dimensions exceed supported range");
    }
    return ldoc::Vec2{w, h};
}

VideoFilter resolve_filter(const ldoc::Effect& e, int w, int h) {
    VideoFilter out;
    const auto size = filter_size(std::span{&e, 1}, w, h);
    if (!size)
        return out;
    out.width = static_cast<int>(size->x);
    out.height = static_cast<int>(size->y);
    auto& p = out.values;
    const auto n = [&](const char* key) { return static_cast<float>(filter_number(e, key)); };
    if (e.type == "filter.color-correction") {
        out.kind = 1;
        p[0] = n("gamma") < 0 ? 1 - n("gamma") : 1 / (1 + n("gamma"));
        p[1] = 1 + n("contrast");
        p[2] = n("brightness");
        p[3] = 1 + n("saturation");
        p[4] = n("hue_shift") * std::numbers::pi_v<float> / 180;
        p[5] = n("opacity");
        for (std::size_t k = 0; k < 3; ++k) {
            const auto shift = static_cast<unsigned>((2 - k) * 8);
            p[6 + k] =
                static_cast<float>((static_cast<unsigned>(n("color_multiply")) >> shift) & 255) /
                255;
            p[9 + k] =
                static_cast<float>((static_cast<unsigned>(n("color_add")) >> shift) & 255) / 255;
        }
    } else if (e.type == "filter.crop") {
        out.kind = 2;
        p[0] = n("left");
        p[1] = n("top");
    } else if (e.type == "filter.scale")
        out.kind = 3;
    else if (e.type == "filter.color-key" || e.type == "filter.chroma-key") {
        out.kind = e.type == "filter.color-key" ? 4 : 5;
        const auto c = static_cast<unsigned>(filter_number(e, "key_color"));
        p[0] = static_cast<float>((c >> 16) & 255) / 255;
        p[1] = static_cast<float>((c >> 8) & 255) / 255;
        p[2] = static_cast<float>(c & 255) / 255;
        p[3] = n("similarity");
        p[4] = n("smoothness");
        p[5] = n("spill");
    } else if (e.type == "filter.luma-key") {
        out.kind = 6;
        p[0] = n("luma_min");
        p[1] = n("luma_max");
        p[2] = n("luma_min_smooth");
        p[3] = n("luma_max_smooth");
    } else if (e.type == "filter.sharpen") {
        out.kind = 7;
        p[0] = n("sharpness");
    } else if (e.type == "filter.mask") {
        out.kind = 8;
        out.auxiliary = mask_image(filter_text(e, "path"));
        p[0] = n("opacity");
        const auto mode = filter_text(e, "mode");
        p[1] = mode == "alpha-color" ? 0
               : mode == "alpha"     ? 1
               : mode == "multiply"  ? 2
               : mode == "add"       ? 3
                                     : 4;
    }
    return out;
}

Picture filter_picture(const Picture& input, const VideoFilter& filter) {
    Picture out;
    out.width = filter.width;
    out.height = filter.height;
    out.stride = static_cast<unsigned>(out.width) * 4;
    out.status = input.status;
    out.gaps = input.gaps;
    out.pixels.resize(static_cast<std::size_t>(out.stride) * static_cast<std::size_t>(out.height));
    const auto& p = filter.values;
    const auto read = [&](int x, int y) {
        std::array<float, 4> c{};
        x = std::clamp(x, 0, input.width - 1);
        y = std::clamp(y, 0, input.height - 1);
        const auto* v = input.read_from() + static_cast<std::size_t>(y) * input.stride +
                        static_cast<std::size_t>(x) * 4;
        for (std::size_t k = 0; k < 4; ++k)
            c[k] = v[k] / 255.0f;
        return c;
    };
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            auto c = read(x, y);
            if (filter.kind == 2) {
                const int sx = x + static_cast<int>(p[0]), sy = y + static_cast<int>(p[1]);
                c = sx < 0 || sy < 0 || sx >= input.width || sy >= input.height
                        ? std::array<float, 4>{}
                        : read(sx, sy);
            } else if (filter.kind == 3)
                c = read(
                    static_cast<int>(static_cast<std::int64_t>(x) * input.width / out.width),
                    static_cast<int>(static_cast<std::int64_t>(y) * input.height / out.height));
            else if (filter.kind == 1) {
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = (std::pow(std::max(c[k], 0.0f), p[0]) - 0.5f) * p[1] + 0.5f + p[2];
                const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = l + (c[k] - l) * p[3];
                const float co = std::cos(p[4]), si = std::sin(p[4]),
                            sum = (c[0] + c[1] + c[2]) * (1 - co) / 3;
                const auto old = c;
                c[0] = old[0] * co + (old[2] - old[1]) * si / std::sqrt(3.0f) + sum;
                c[1] = old[1] * co + (old[0] - old[2]) * si / std::sqrt(3.0f) + sum;
                c[2] = old[2] * co + (old[1] - old[0]) * si / std::sqrt(3.0f) + sum;
                c[3] *= p[5];
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] = c[k] * p[6 + k] + p[9 + k];
            } else if (filter.kind == 4 || filter.kind == 5) {
                float distance = 0;
                if (filter.kind == 4)
                    for (std::size_t k = 0; k < 3; ++k)
                        distance += (c[k] - p[k]) * (c[k] - p[k]);
                else {
                    const float u = -0.168736f * (c[0] - p[0]) - 0.331264f * (c[1] - p[1]) +
                                    0.5f * (c[2] - p[2]);
                    const float v = 0.5f * (c[0] - p[0]) - 0.418688f * (c[1] - p[1]) -
                                    0.081312f * (c[2] - p[2]);
                    distance = u * u + v * v;
                }
                const float base = std::sqrt(distance) - p[3];
                c[3] *= std::pow(clamp(base / p[4]), 1.5f);
                if (filter.kind == 5) {
                    const float spill = std::pow(clamp(base / p[5]), 1.5f),
                                l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                    for (std::size_t k = 0; k < 3; ++k)
                        c[k] = l + (c[k] - l) * spill;
                }
            } else if (filter.kind == 6) {
                const float l = c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f;
                c[3] *= (p[2] > 0 ? clamp((l - p[0]) / p[2]) : float(l >= p[0])) *
                        (p[3] > 0 ? clamp((p[1] - l) / p[3]) : float(l <= p[1]));
            } else if (filter.kind == 7) {
                const auto a = read(x - 1, y), b = read(x + 1, y), d = read(x, y - 1),
                           e = read(x, y + 1);
                for (std::size_t k = 0; k < 3; ++k)
                    c[k] += p[0] * (4 * c[k] - a[k] - b[k] - d[k] - e[k]);
            } else if (filter.kind == 8 && filter.auxiliary) {
                const auto& m = *filter.auxiliary;
                const int mx = static_cast<int>(static_cast<std::int64_t>(x) * m.width / out.width),
                          my = static_cast<int>(static_cast<std::int64_t>(y) * m.height /
                                                out.height);
                const auto* v = m.read_from() + static_cast<std::size_t>(my) * m.stride +
                                static_cast<std::size_t>(mx) * 4;
                if (p[1] < 2)
                    c[3] *= (p[1] == 0 ? (v[0] * 0.2126f + v[1] * 0.7152f + v[2] * 0.0722f) / 255
                                       : v[3] / 255.0f) *
                            p[0];
                else
                    for (std::size_t k = 0; k < 3; ++k) {
                        const float mask = v[k] / 255.0f;
                        const float changed = p[1] == 2   ? c[k] * mask
                                              : p[1] == 3 ? c[k] + mask
                                                          : c[k] - mask;
                        c[k] += (changed - c[k]) * p[0];
                    }
            }
            auto* dst = out.pixels.data() + static_cast<std::size_t>(y) * out.stride +
                        static_cast<std::size_t>(x) * 4;
            for (std::size_t k = 0; k < 4; ++k)
                dst[k] = static_cast<std::uint8_t>(std::lround(clamp(c[k]) * 255));
        }
    return out;
}
} // namespace lcomp
