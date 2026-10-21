// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QFont>
#include <QFontMetrics>
#include <QImageReader>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <json/json.h>
#include <lcomp/composite.hpp>
#include <ldoc/project_io.hpp>
#include <limport/obs.hpp>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace limport {
namespace {
namespace fs = std::filesystem;
using J = Json::Value;

std::string str(const J& v, const char* key, std::string fallback = {}) {
    if (!v.isMember(key))
        return fallback;
    if (!v[key].isString())
        throw std::runtime_error(std::string(key) + " must be a string");
    return v[key].asString();
}

double num(const J& v, const char* key, double fallback = 0) {
    if (!v.isMember(key))
        return fallback;
    if (!v[key].isNumeric())
        throw std::runtime_error(std::string(key) + " must be numeric");
    const double n = v[key].asDouble();
    if (!std::isfinite(n) || std::abs(n) > 1e12)
        throw std::runtime_error(std::string(key) + " is out of range");
    return n;
}

bool flag(const J& v, const char* key, bool fallback = false) {
    if (!v.isMember(key))
        return fallback;
    if (!v[key].isBool())
        throw std::runtime_error(std::string(key) + " must be boolean");
    return v[key].asBool();
}

int integer(const J& v, const char* key, int fallback, int low, int high) {
    const double n = num(v, key, fallback);
    if (n < low || n > high || n != std::floor(n))
        throw std::runtime_error(std::string(key) + " is out of range or not integral");
    return static_cast<int>(n);
}

std::string json(const J& v) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, v);
}

ldoc::Rgba colour(const J& v, const char* key, std::uint32_t fallback = 0xffffffff) {
    const double n = num(v, key, fallback);
    if (n < 0 || n > 4294967295.0 || n != std::floor(n))
        throw std::runtime_error("invalid OBS packed colour");
    const auto c = static_cast<std::uint32_t>(n);
    return ldoc::Rgba::from_srgb8(c & 255, (c >> 8) & 255, (c >> 16) & 255,
                                  static_cast<std::uint8_t>(c >> 24));
}

struct Entry {
    J original;
    ldoc::Id id = ldoc::Id::generate();
    std::string name;
    std::string type;
    ldoc::Vec2 size;
    std::optional<ldoc::Id> bus;
    bool scene = false;
    bool global = false;
    bool ignored = false;
};

struct Converter {
    ObsResult result;
    fs::path base;
    std::vector<Entry> entries;
    std::map<std::string, std::size_t> uuids;
    std::map<std::string, std::vector<std::size_t>> names;
    std::size_t item_count = 0;
    std::size_t expanded_nodes = 0;
    std::map<std::string, std::size_t> canvas_indices;

    std::size_t canvas_index(const Entry& e) const {
        const auto uuid = str(e.original, "canvas_uuid");
        const auto found = canvas_indices.find(uuid);
        return found == canvas_indices.end() ? 0 : found->second;
    }

    void canvases(const J& root) {
        std::map<std::string, std::string> labels;
        if (root.isMember("canvases") && !root["canvases"].isArray())
            throw std::runtime_error("canvases must be an array");
        for (const auto& canvas : root["canvases"]) {
            const auto& info = canvas["info"];
            const auto uuid = str(info, "uuid");
            if (uuid == "6c69626f-6273-4c00-9d88-c5136d61696e")
                continue;
            if (uuid.empty() || !labels.emplace(uuid, str(info, "name", uuid)).second)
                throw std::runtime_error("missing or duplicate OBS canvas UUID");
        }
        for (const auto& e : entries) {
            const auto uuid = str(e.original, "canvas_uuid");
            if (uuid.empty() || uuid == "6c69626f-6273-4c00-9d88-c5136d61696e")
                continue;
            labels.try_emplace(uuid, "OBS canvas " + uuid);
        }
        if (labels.size() > 64)
            throw std::runtime_error("too many OBS canvases");
        for (const auto& [uuid, name] : labels) {
            std::optional<std::pair<int, int>> dimensions;
            for (const auto& e : entries) {
                if (e.type != "scene" || str(e.original, "canvas_uuid") != uuid)
                    continue;
                const auto& settings = e.original["settings"];
                if (!flag(settings, "custom_size"))
                    continue;
                const auto size = std::pair{integer(settings, "cx", 0, 1, 8192),
                                            integer(settings, "cy", 0, 1, 8192)};
                if (static_cast<std::int64_t>(size.first) * size.second > 33554432)
                    throw std::runtime_error(name + ": canvas exceeds 32M pixels");
                if (dimensions && dimensions != size)
                    throw std::runtime_error(
                        name +
                        ": conflicting scene dimensions; choose canvas geometry before import");
                dimensions = size;
            }
            auto canvas = result.project.canvases.front();
            canvas.id = ldoc::Id::generate();
            canvas.name = name;
            canvas.emergency_scene.reset();
            if (dimensions) {
                canvas.width = dimensions->first;
                canvas.height = dimensions->second;
            } else {
                result.warnings.push_back(name + ": canvas dimensions absent; separate canvas uses "
                                                 "supplied Main dimensions; review geometry");
            }
            result.warnings.push_back(name + ": canvas FPS inherits operator-supplied Main rate");
            canvas_indices[uuid] = result.project.canvases.size();
            result.project.canvases.push_back(std::move(canvas));
        }
    }

    void warn(const Entry& e, std::string message) {
        result.warnings.push_back(e.name + ": " + message);
    }

    std::string path(const Entry& e, std::string name) {
        if (name.empty()) {
            warn(e, "file is not selected");
            return name;
        }
        if (name.find("://") != std::string::npos) {
            warn(e, "remote media retained; import does not fetch it");
            return name;
        }
        fs::path p(name);
        if (name.size() > 2 && name[1] == ':') {
            warn(e, "Windows path needs relocation: " + name);
            return name;
        }
        if (p.is_relative())
            p = base / p;
        p = p.lexically_normal();
        std::error_code error;
        if (!fs::is_regular_file(p, error))
            warn(e, "missing file: " + p.string());
        return p.string();
    }

    ldoc::Vec2 image_size(const std::string& file) {
        if (file.find("://") != std::string::npos)
            return {};
        QImageReader reader(QString::fromStdString(file));
        const auto size = reader.size();
        if (size.width() <= 0 || size.height() <= 0 || size.width() > 8192 || size.height() > 8192)
            return {};
        return {static_cast<double>(size.width()), static_cast<double>(size.height())};
    }

    std::size_t reference(const J& item, bool main_scene = false) const {
        const auto uuid = str(item, "source_uuid");
        if (!uuid.empty()) {
            auto found = uuids.find(uuid);
            if (found == uuids.end())
                throw std::runtime_error("missing source UUID: " + uuid);
            return found->second;
        }
        const auto name = str(item, "name");
        auto found = names.find(name);
        if (main_scene && found != names.end()) {
            std::optional<std::size_t> selected;
            for (auto index : found->second) {
                const auto& e = entries[index];
                const auto canvas = str(e.original, "canvas_uuid");
                if (e.type == "scene" &&
                    (canvas.empty() || canvas == "6c69626f-6273-4c00-9d88-c5136d61696e")) {
                    if (selected)
                        throw std::runtime_error("ambiguous main-canvas scene: " + name);
                    selected = index;
                }
            }
            if (selected)
                return *selected;
        }
        if (found == names.end() || found->second.size() != 1)
            throw std::runtime_error("missing or ambiguous source name: " + name);
        return found->second.front();
    }

    void add(const J& source, bool global = false) {
        if (!source.isObject())
            throw std::runtime_error("source must be an object");
        if (entries.size() >= 10000)
            throw std::runtime_error("too many OBS sources");
        Entry e;
        e.original = source;
        e.name = str(source, "name");
        e.type = str(source, "id");
        if (e.name.empty() || e.type.empty())
            throw std::runtime_error("source needs name and id");
        if (source.isMember("settings") && !source["settings"].isObject())
            throw std::runtime_error(e.name + ": settings must be an object");
        e.global = global;
        e.scene = e.type == "scene" || e.type == "group";
        e.ignored = e.type == "streamelements_transition_audio_wrapper_source";
        const auto uuid = str(source, "uuid");
        if (!uuid.empty() && !uuids.emplace(uuid, entries.size()).second)
            throw std::runtime_error("duplicate source UUID: " + uuid);
        names[e.name].push_back(entries.size());
        entries.push_back(std::move(e));
    }

    void source(Entry& e) {
        if (e.ignored) {
            warn(e, "obsolete StreamElements transition audio wrapper ignored");
            return;
        }
        const auto& s = e.original["settings"];
        const auto& canvas = result.project.canvases[canvas_index(e)];
        e.size = {static_cast<double>(canvas.width), static_cast<double>(canvas.height)};
        if (e.scene) {
            if (flag(s, "custom_size") || e.type == "group") {
                if (num(s, "cx") > 0 && num(s, "cy") > 0)
                    e.size = {static_cast<double>(integer(s, "cx", canvas.width, 1, 8192)),
                              static_cast<double>(integer(s, "cy", canvas.height, 1, 8192))};
                else
                    warn(e, "group bounds missing; uses canvas bounds; check placement");
            }
            if (e.size.x * e.size.y > 33554432)
                throw std::runtime_error(e.name + ": group surface exceeds 32M pixels");
            return;
        }
        ldoc::Source out;
        out.id = e.id;
        out.name = e.name;
        out.settings["obs.original"] = json(e.original);
        auto& p = out.settings;
        const auto version = str(e.original, "versioned_id", e.type);
        bool audio = false;
        if (e.type.starts_with("color_source")) {
            out.type = "source.color";
            const bool old = version == "color_source";
            p["color"] = colour(s, "color", version == "color_source_v3" ? 0xffd1d1d1 : 0xffffffff);
            e.size = {static_cast<double>(integer(s, "width", old ? 400 : 1920, 1, 8192)),
                      static_cast<double>(integer(s, "height", old ? 400 : 1080, 1, 8192))};
        } else if (e.type == "image_source") {
            out.type = "source.image";
            const auto file = path(e, str(s, "file"));
            p["file"] = file;
            p["unloadWhenHidden"] = flag(s, "unload");
            p["alphaLinear"] = flag(s, "linear_alpha");
            const auto measured = image_size(file);
            if (measured.x > 0)
                e.size = measured;
            else
                warn(e, "image dimensions unavailable; placement uses canvas dimensions until "
                        "repaired");
        } else if (e.type.starts_with("text_ft2_source") || e.type.starts_with("text_gdiplus")) {
            out.type = "source.text";
            const auto& font = s["font"];
            const int size =
                integer(font, "size", version == "text_ft2_source_v2" ? 256 : 32, 1, 1024);
            std::string text = str(s, "text");
            if (flag(s, "from_file") || flag(s, "read_from_file"))
                warn(e, "text-file watching is unsupported; saved inline text retained");
            p["text"] = text;
            p["font"] = str(font, "face", "Sans");
            p["size"] = static_cast<std::int64_t>(size);
            p["color"] = colour(s, e.type.starts_with("text_ft2") ? "color1" : "color");
            if (e.type.starts_with("text_gdiplus"))
                std::get<ldoc::Rgba>(p["color"]).a = integer(s, "opacity", 100, 0, 100) / 100.0;
            p["alignment"] = std::string("Left");
            QFont qfont(QString::fromStdString(std::get<std::string>(p["font"])));
            qfont.setPixelSize(size);
            const auto rect = QFontMetrics(qfont).boundingRect(QRect(0, 0, 4096, 4096),
                                                               Qt::AlignLeft | Qt::AlignTop,
                                                               QString::fromStdString(text));
            e.size = {static_cast<double>(std::clamp(rect.width() + 4, 1, 4096)),
                      static_cast<double>(std::clamp(rect.height() + 4, 1, 4096))};
            warn(e, "Qt font layout replaces OBS text; check font, wrapping, gradients, outline "
                    "and shadow");
        } else if (e.type == "browser_source") {
            out.type = "source.browser";
            audio = true;
            auto url = str(s, "url", "https://obsproject.com/browser-source");
            if (flag(s, "is_local_file"))
                url = QUrl::fromLocalFile(QString::fromStdString(path(e, str(s, "local_file"))))
                          .toString()
                          .toStdString();
            p["url"] = url;
            p["css"] = str(
                s, "css", "body { background-color: rgba(0,0,0,0); margin: 0; overflow: hidden; }");
            p["fps"] = static_cast<std::int64_t>(integer(s, "fps", 30, 1, 60));
            p["transparentBackground"] = true;
            e.size = {static_cast<double>(integer(s, "width", 800, 1, 4096)),
                      static_cast<double>(integer(s, "height", 600, 1, 4096))};
            warn(e,
                 "browser reloads when shown; OBS browser APIs and session state are not imported");
        } else if (e.type == "ffmpeg_source" || e.type == "vlc_source" || e.type == "slideshow") {
            audio = e.type != "slideshow";
            out.type = e.type == "slideshow"    ? "source.slideshow"
                       : e.type == "vlc_source" ? "source.playlist"
                                                : "source.media";
            if (e.type == "ffmpeg_source") {
                p["localFile"] =
                    path(e, str(s, flag(s, "is_local_file", true) ? "local_file" : "input"));
                p["loop"] = flag(s, "looping");
            } else {
                const auto& files = s[e.type == "slideshow" ? "files" : "playlist"];
                if (!files.isNull() && !files.isArray())
                    throw std::runtime_error(e.name + ": files must be an array");
                std::string joined;
                ldoc::Vec2 largest{};
                for (const auto& file : files) {
                    const auto name = path(e, str(file, "value"));
                    if (name.find('\n') != std::string::npos)
                        throw std::runtime_error("newline in playlist path");
                    if (!joined.empty())
                        joined += '\n';
                    joined += name;
                    const auto size = image_size(name);
                    largest.x = std::max(largest.x, size.x);
                    largest.y = std::max(largest.y, size.y);
                }
                if (largest.x > 0 && largest.y > 0)
                    e.size = largest;
                p["files"] = joined;
                p["loop"] = flag(s, "loop", true);
            }
            p["clearOnEnd"] = flag(s, "clear_on_media_end", true);
            p["speedPercent"] =
                static_cast<std::int64_t>(integer(s, "speed_percent", 100, 25, 400));
            p["intervalMs"] =
                static_cast<std::int64_t>(integer(s, "slide_time", 8000, 100, 100000));
            warn(e, "check output dimensions and aspect fit; playback restarts when shown; "
                    "shuffle/transitions are not imported");
        } else if (e.type == "pulse_input_capture" || e.type == "pulse_output_capture" ||
                   e.type == "wasapi_input_capture" || e.type == "wasapi_output_capture") {
            const bool output = e.type.find("output") != std::string::npos;
            out.type = output ? "source.audio-output" : "source.audio-input";
            audio = true;
            auto device = str(s, "device_id", "default");
            if (e.type.starts_with("wasapi")) {
                device = "obs-unresolved-device";
                warn(e, "select a Linux audio device; Windows identity cannot be mapped");
            } else
                warn(e, "verify audio device on this machine");
            p["device"] = device;
        } else if (e.type == "v4l2_input" || e.type == "dshow_input") {
            out.type = "source.v4l2";
            p["device"] = e.type == "v4l2_input" ? str(s, "device_id") : std::string{};
            p["resolution"] = std::string("Leave unchanged");
            p["frameRate"] = std::string("Leave unchanged");
            p["colourRange"] = std::string("Default");
            p["buffering"] = flag(s, "buffering", true);
            p["autoReset"] = flag(s, "auto_reset");
            p["timeoutFrames"] = static_cast<std::int64_t>(integer(s, "timeout_frames", 5, 2, 120));
            p["videoFormat"] = std::string("Auto");
            const double format = num(s, "pixelformat", -1);
            if (format != -1) {
                if (format < 0 || format > 4294967295.0 || format != std::floor(format))
                    throw std::runtime_error("invalid V4L2 pixel format");
                const auto code = static_cast<std::uint32_t>(format);
                std::string text(4, ' ');
                for (unsigned i = 0; i < 4; ++i) text[i] = static_cast<char>((code >> (8 * i)) & 0xff);
                p["videoFormat"] = std::move(text);
            }
            const auto unpack = [](const J& settings, const char* key) {
                if (!settings.isMember(key))
                    return std::pair<int, int>{0, 0};
                if (!settings[key].isInt64())
                    throw std::runtime_error("invalid V4L2 packed mode");
                const auto value = settings[key].asInt64();
                if (value <= 0)
                    return std::pair<int, int>{0, 0};
                const auto high = value >> 32;
                const auto a = high ? high : value >> 16;
                const auto b = value & (high ? 0xffffffffLL : 0xffffLL);
                if (a > 1000000 || b > 1000000)
                    throw std::runtime_error("V4L2 mode out of range");
                return std::pair{static_cast<int>(a), static_cast<int>(b)};
            };
            const auto [width, height] = unpack(s, "resolution");
            if (width > 0 && height > 0) {
                if (width > 8192 || height > 8192)
                    throw std::runtime_error("V4L2 dimensions out of range");
                p["resolution"] = std::to_string(width) + "x" + std::to_string(height);
                e.size = {static_cast<double>(width), static_cast<double>(height)};
            }
            const auto [den, rate] = unpack(s, "framerate");
            if (den > 0 && rate > 0) {
                if (rate % den == 0 && rate / den <= 240)
                    p["frameRate"] = std::to_string(rate / den);
                else if (rate == 60000 && den == 1001)
                    p["frameRate"] = std::string("59.94");
                else if (rate == 30000 && den == 1001)
                    p["frameRate"] = std::string("29.97");
                else
                    warn(e, "capture frame rate cannot be represented; select it manually");
            }
            warn(e, "verify video device, resolution, frame rate and colour range; select audio "
                    "separately");
        } else if (e.type == "decklink-input") {
            out.type = "source.blackmagic";
            audio = true;
            p["deviceIndex"] = std::int64_t{-1};
            p["mode"] = std::string{};
            warn(e, "select Blackmagic device index and exact progressive SDR mode; vendor "
                    "identity not portable");
        } else if (e.type == "monitor_capture" || e.type == "window_capture" ||
                   e.type == "game_capture" || e.type == "xshm_input" ||
                   e.type == "xcomposite_input" || e.type == "pipewire-desktop-capture-source" ||
                   e.type == "pipewire-window-capture-source" ||
                   e.type == "pipewire-screen-capture-source") {
            const bool window =
                e.type.find("window") != std::string::npos || e.type == "xcomposite_input";
            const bool game = e.type.find("game") != std::string::npos;
            out.type = game ? "source.game" : window ? "source.window" : "source.screen";
            p[window || game ? "window" : "output"] = std::string{};
            p["showCursor"] = flag(s, "show_cursor", flag(s, "capture_cursor", true));
            p["pauseWhenHidden"] = true;
            warn(e, "select a Wayland display/window; OBS capture identity is not reused");
        } else {
            out.type = "source.obs-unsupported";
            warn(e, "unsupported source type " + e.type +
                        "; original record retained, no pixels/audio");
        }
        if ((out.type == "source.media" || out.type == "source.playlist" ||
             out.type == "source.slideshow" || out.type == "source.blackmagic") &&
            (e.size.x > 4096 || e.size.y > 4096)) {
            warn(e, "worker output limited to 4096 per edge; review placement");
            e.size.x = std::min(e.size.x, 4096.0);
            e.size.y = std::min(e.size.y, 4096.0);
        }
        p["width"] = static_cast<std::int64_t>(e.size.x);
        p["height"] = static_cast<std::int64_t>(e.size.y);
        if (audio) {
            ldoc::Bus bus;
            bus.id = ldoc::Id::generate();
            bus.name = e.name + " (OBS audio)";
            bus.kind = ldoc::BusKind::Input;
            const double volume = num(e.original, "volume", 1);
            if (volume < 0 || volume > 16)
                throw std::runtime_error(e.name + ": invalid volume");
            bus.gain_db = volume > 0 ? 20 * std::log10(volume) : -120;
            bus.mute = flag(e.original, "muted") || volume == 0;
            bus.pan = std::clamp(num(e.original, "balance", 0.5) * 2 - 1, -1.0, 1.0);
            const double sync = num(e.original, "sync") / 1e6;
            if (sync < 0 || sync > 10000)
                warn(e, "audio sync outside 0..10000 ms; clamped");
            bus.delay_ms = std::clamp(sync, 0.0, 10000.0);
            const int monitoring = integer(e.original, "monitoring_type", 0, 0, 2);
            // Import never starts local monitoring unexpectedly (especially an output loopback).
            if (monitoring)
                warn(e, "local monitoring left off; review before enabling");
            const int tracks = integer(e.original, "mixers", 255, 0, 255);
            if (monitoring != 1) {
                for (int track = 0; track < 6; ++track)
                    if ((tracks & (1 << track)) != 0)
                        bus.sends.push_back(
                            {result.project.buses[static_cast<std::size_t>(track)].id, 0, false});
            }
            e.bus = bus.id;
            result.project.buses.push_back(std::move(bus));
        }
        result.project.sources.push_back(std::move(out));
    }

    ldoc::Node node(const J& item, Entry& e) {
        if (++expanded_nodes > 100000)
            throw std::runtime_error("expanded OBS groups exceed 100000 nodes");
        auto n = ldoc::make_node(e.type == "group" ? ldoc::NodeKind::Group
                                 : e.scene         ? ldoc::NodeKind::Precomp
                                                   : ldoc::NodeKind::Source,
                                 e.name);
        if (e.type == "group") {
            auto& group = std::get<ldoc::GroupData>(n.data);
            group.size = e.size;
            group.isolate = true;
            const auto& items = e.original["settings"]["items"];
            for (auto it = items.end(); it != items.begin();) {
                --it;
                if (flag(*it, "group_item_backup"))
                    continue;
                auto& child = entries[reference(*it)];
                if (!child.ignored)
                    group.children.push_back(node(*it, child));
            }
        } else if (e.scene)
            std::get<ldoc::PrecompData>(n.data) = {e.id, e.size, ldoc::ColorSpace::Rec709};
        else {
            auto& data = std::get<ldoc::SourceData>(n.data);
            data.source = e.id;
            data.audio.bus = e.bus;
        }
        n.visible = flag(item, "visible", true);
        n.locked = flag(item, "locked");
        n.layout.crop = {static_cast<double>(integer(item, "crop_left", 0, 0, 8192)),
                         static_cast<double>(integer(item, "crop_top", 0, 0, 8192)),
                         static_cast<double>(integer(item, "crop_right", 0, 0, 8192)),
                         static_cast<double>(integer(item, "crop_bottom", 0, 0, 8192))};
        auto& t = n.transform;
        t.pivot = {0, 0};
        t.scale = {num(item["scale"], "x", 1), num(item["scale"], "y", 1)};
        if (std::abs(t.scale.x) > 10000 || std::abs(t.scale.y) > 10000)
            throw std::runtime_error(e.name + ": scale out of range");
        t.rotation_deg = num(item, "rot");
        if (!lcomp::quarter_turn(t.rotation_deg))
            warn(e, "non-quarter rotation retained but renderer cannot draw it");
        const int align = integer(item, "align", 5, 0, 15);
        const auto aligned = [](double v, int a, int low, int high) {
            if (!std::isfinite(v) || std::abs(v) > 1e7)
                throw std::runtime_error("placement exceeds supported range");
            const int pixels = static_cast<int>(v);
            return static_cast<double>((a & high) ? pixels : (a & low) ? 0 : pixels / 2);
        };
        const double sw = e.size.x - n.layout.crop.left - n.layout.crop.right;
        const double sh = e.size.y - n.layout.crop.top - n.layout.crop.bottom;
        if (sw <= 0 || sh <= 0)
            warn(e, "crop removes entire source; no picture until corrected");
        double ox = 0, oy = 0;
        const int bounds_type = integer(item, "bounds_type", 0, 0, 6);
        if (bounds_type && sw > 0 && sh > 0 && t.scale.x != 0 && t.scale.y != 0) {
            const double bw = num(item["bounds"], "x"), bh = num(item["bounds"], "y");
            if (bw <= 0 || bh <= 0 || bw > 1e7 || bh > 1e7)
                throw std::runtime_error(e.name + ": invalid bounds");
            const double w = sw * std::abs(t.scale.x), h = sh * std::abs(t.scale.y);
            double factor = 1;
            if (bounds_type == 1)
                t.scale = {std::copysign(bw / sw, t.scale.x), std::copysign(bh / sh, t.scale.y)};
            else {
                if (bounds_type == 2)
                    factor = std::min(bw / w, bh / h);
                if (bounds_type == 3)
                    factor = std::max(bw / w, bh / h);
                if (bounds_type == 4)
                    factor = bw / w;
                if (bounds_type == 5)
                    factor = bh / h;
                if (bounds_type == 6)
                    factor = std::min({1.0, bw / w, bh / h});
                t.scale.x *= factor;
                t.scale.y *= factor;
            }
            const int ba = integer(item, "bounds_align", 0, 0, 15);
            ox = aligned(-(bw - std::abs(sw * t.scale.x)), ba, 1, 2) +
                 std::min(0.0, sw * t.scale.x) + aligned(bw, align, 1, 2);
            oy = aligned(-(bh - std::abs(sh * t.scale.y)), ba, 4, 8) +
                 std::min(0.0, sh * t.scale.y) + aligned(bh, align, 4, 8);
            if (flag(item, "bounds_crop"))
                warn(e, "crop-to-bounds is not applied; check overflow");
        } else {
            ox = aligned(sw * t.scale.x, align, 1, 2);
            oy = aligned(sh * t.scale.y, align, 4, 8);
        }
        ox += n.layout.crop.left * t.scale.x;
        oy += n.layout.crop.top * t.scale.y;
        const double angle = t.rotation_deg * std::acos(-1.0) / 180;
        t.position = {num(item["pos"], "x") - (ox * std::cos(angle) - oy * std::sin(angle)),
                      num(item["pos"], "y") - (ox * std::sin(angle) + oy * std::cos(angle))};
        if (std::abs(t.position.x) > 1e7 || std::abs(t.position.y) > 1e7)
            throw std::runtime_error(e.name + ": position out of range");
        const auto blend = str(item, "blend_type", "normal");
        const auto mode =
            ldoc::blend_mode_from_string(blend == "additive" ? "linear-dodge" : blend);
        if (mode && lcomp::blend_supported(*mode)) {
            n.blend.mode = *mode;
            if (*mode != ldoc::BlendMode::Normal)
                warn(e, "blend mode converted; native source-over alpha may differ from OBS "
                        "for transparent layers");
        } else {
            warn(e, "unsupported blend mode retained in original; uses normal blend");
        }
        if (str(item, "blend_method", "default") != "default")
            warn(e, "nonlinear blend method not converted; uses scene-linear blend");
        if (str(item, "scale_filter", "disable") != "disable")
            warn(e, "scaling filter not converted; uses native sampler");
        return n;
    }

    void run(const J& root) {
        if (!root.isObject() || !root["sources"].isArray())
            throw std::runtime_error("OBS collection needs a sources array");
        result.project.name = str(root, "name", "Imported OBS collection");
        result.project.scenes.clear();
        result.project.buses.resize(1);
        for (int track = 2; track <= 6; ++track) {
            ldoc::Bus bus;
            bus.id = ldoc::Id::generate();
            bus.name = "OBS track " + std::to_string(track);
            bus.kind = ldoc::BusKind::Track;
            result.project.buses.push_back(bus);
        }
        for (const auto& s : root["sources"])
            add(s);
        if (root.isMember("groups") && !root["groups"].isArray())
            throw std::runtime_error("groups must be an array");
        for (const auto& s : root["groups"])
            add(s);
        for (const char* key : {"DesktopAudioDevice1", "DesktopAudioDevice2", "AuxAudioDevice1",
                                "AuxAudioDevice2", "AuxAudioDevice3", "AuxAudioDevice4"})
            if (root.isMember(key) && !root[key].isNull())
                add(root[key], true);
        canvases(root);
        for (auto& e : entries) {
            source(e);
            if (!e.original["filters"].empty())
                warn(e, "filters not applied; original filter records retained");
            if (num(e.original, "deinterlace_mode") != 0)
                warn(e, "deinterlacing not applied");
        }
        // Validate references and bound recursive scene evaluation before constructing any output.
        std::vector<int> state(entries.size());
        std::vector<int> heights(entries.size());
        const auto check = [&](const auto& self, std::size_t i, int depth) -> void {
            if (depth > 64 || state[i] == 1)
                throw std::runtime_error("cyclic or excessively nested OBS scenes");
            if (depth + heights[i] > 64)
                throw std::runtime_error("excessively nested OBS scenes");
            if (state[i] == 2 || !entries[i].scene)
                return;
            state[i] = 1;
            const auto& items = entries[i].original["settings"]["items"];
            if (!items.isNull() && !items.isArray())
                throw std::runtime_error("scene items must be an array");
            for (const auto& item : items) {
                if (++item_count > 100000)
                    throw std::runtime_error("too many scene items");
                if (!item.isObject())
                    throw std::runtime_error("scene item must be an object");
                if (!flag(item, "group_item_backup")) {
                    const auto child = reference(item);
                    self(self, child, depth + 1);
                    heights[i] = std::max(heights[i], heights[child] + 1);
                }
            }
            if (depth + heights[i] > 64)
                throw std::runtime_error("excessively nested OBS scenes");
            state[i] = 2;
        };
        for (std::size_t i = 0; i < entries.size(); ++i)
            check(check, i, 0);
        std::vector<std::size_t> order;
        if (root.isMember("scene_order") && !root["scene_order"].isArray())
            throw std::runtime_error("scene_order must be an array");
        for (const auto& row : root["scene_order"]) {
            const auto i = reference(row, true);
            if (!entries[i].scene)
                throw std::runtime_error("scene order names a non-scene source");
            if (entries[i].type == "scene" && std::ranges::find(order, i) == order.end())
                order.push_back(i);
        }
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].type == "scene" && std::ranges::find(order, i) == order.end())
                order.push_back(i);
        for (auto i : order) {
            auto& e = entries[i];
            ldoc::Scene scene;
            scene.id = e.id;
            scene.name = e.name;
            scene.canvas = result.project.canvases[canvas_index(e)].id;
            const auto& items = e.original["settings"]["items"];
            for (auto it = items.end(); it != items.begin();) {
                --it;
                if (!flag(*it, "group_item_backup")) {
                    auto& child = entries[reference(*it)];
                    if (!child.ignored)
                        scene.roots.push_back(node(*it, child));
                }
            }
            for (auto& global : entries)
                if (global.global && !global.ignored)
                    scene.roots.push_back(node(J(Json::objectValue), global));
            result.project.scenes.push_back(std::move(scene));
        }
        if (result.project.scenes.empty())
            throw std::runtime_error("collection contains no scenes");
        auto& deck = result.project.decks.front();
        const auto select = [&](const std::string& name) -> ldoc::Id {
            if (name.empty())
                return result.project.scenes.front().id;
            J ref;
            ref["name"] = name;
            const auto& e = entries[reference(ref, true)];
            if (e.type != "scene")
                throw std::runtime_error("active scene names a non-scene source");
            return e.id;
        };
        deck.program_scene = select(str(root, "current_program_scene", str(root, "current_scene")));
        deck.preview_scene = select(str(root, "current_scene"));
        for (std::size_t i = 1; i < result.project.canvases.size(); ++i) {
            const auto& canvas = result.project.canvases[i];
            ldoc::Deck extra;
            extra.id = ldoc::Id::generate();
            extra.name = canvas.name;
            extra.canvas = canvas.id;
            for (const auto index : order) {
                const auto& e = entries[index];
                if (e.type == "scene" && canvas_index(e) == i) {
                    extra.program_scene = extra.preview_scene = e.id;
                    result.warnings.push_back(
                        canvas.name + ": active scene absent; initialized Program/Preview to " +
                        e.name);
                    break;
                }
            }
            result.project.decks.push_back(std::move(extra));
        }
        result.warnings.push_back("OBS track 1 maps to Program; tracks 2-6 are separate buses. "
                                  "Verify active stream/recording tracks in the OBS profile.");
        result.warnings.push_back("Shared audio placements, including nested scenes with global "
                                  "audio, can sum more than once; review routes before use.");
        result.warnings.push_back(
            "Transitions use Cut. Hotkeys, outputs, plugins and unmapped source "
            "options are retained only in obs-original.json.");
    }
};

void write(const fs::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    stream.close();
    if (!stream)
        throw std::runtime_error("cannot write " + path.string());
}
} // namespace

std::expected<ObsResult, std::string>
import_obs(const fs::path& collection, const fs::path& destination, const ObsOptions& options) {
    try {
        if (options.width < 1 || options.width > 8192 || options.height < 1 ||
            options.height > 8192 ||
            static_cast<std::int64_t>(options.width) * options.height > 33554432 ||
            !options.fps.valid() || options.fps.hz() > 240)
            throw std::runtime_error("explicit canvas dimensions (up to 8192, 32M pixels) and rate "
                                     "(0..240 fps) required");
        if (fs::exists(destination))
            throw std::runtime_error("destination already exists");
        const auto bytes = fs::file_size(collection);
        if (bytes == 0 || bytes > 64 * 1024 * 1024)
            throw std::runtime_error("collection must be 1 byte to 64 MiB");
        std::ifstream input(collection, std::ios::binary);
        std::string original(static_cast<std::size_t>(bytes), '\0');
        input.read(original.data(), static_cast<std::streamsize>(bytes));
        if (!input || input.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("collection changed or cannot be read");
        Json::CharReaderBuilder reader;
        reader["rejectDupKeys"] = true;
        reader["failIfExtra"] = true;
        reader["stackLimit"] = 128;
        std::unique_ptr<Json::CharReader> parser(reader.newCharReader());
        J root;
        std::string error;
        if (!parser->parse(original.data(), original.data() + original.size(), &root, &error))
            throw std::runtime_error("invalid OBS JSON: " + error);
        Converter converter;
        converter.base = fs::absolute(collection).parent_path();
        converter.result.project = ldoc::make_default_project("OBS import");
        auto& canvas = converter.result.project.canvases.front();
        canvas.width = options.width;
        canvas.height = options.height;
        canvas.fps = options.fps;
        converter.run(root);
        const auto& main_canvas = converter.result.project.canvases.front();
        std::string report = "# OBS Import Report\n\nCanvas: " + std::to_string(main_canvas.width) +
                             "x" + std::to_string(main_canvas.height) + " at " +
                             std::to_string(main_canvas.fps.num) + "/" +
                             std::to_string(main_canvas.fps.den) + " fps (operator supplied).\n\n";
        for (const auto& warning : converter.result.warnings)
            report += "- " + warning + "\n";
        // Atomic reservation refuses existing directories and dangling symlinks, including races.
        if (!fs::create_directory(destination))
            throw std::runtime_error("destination already exists");
        write(destination / "obs-original.json", original);
        write(destination / "obs-import-report.md", report);
        auto saved = ldoc::create_project(converter.result.project, destination);
        if (!saved)
            throw std::runtime_error(saved.error());
        return std::move(converter.result);
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}
} // namespace limport
