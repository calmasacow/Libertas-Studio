// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include <QImageReader>
#include <QUrl>
#include <QUuid>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <json/json.h>
#include <lcomp/composite.hpp>
#include <lcore/atomic_file.hpp>
#include <limport/obs.hpp>
#include <map>
#include <set>
#include <sstream>
#include <unistd.h>

namespace limport {
namespace {
namespace fs = std::filesystem;
using J = Json::Value;

std::string uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

J vector(double x, double y) {
    J v;
    v["x"] = x;
    v["y"] = y;
    return v;
}

template <class T>
T setting(const ldoc::Source& source, const char* key, T fallback) {
    const auto it = source.settings.find(key);
    if (it != source.settings.end())
        if (const auto* value = std::get_if<T>(&it->second))
            return *value;
    return fallback;
}

struct Record {
    std::string name;
    std::string uuid;
    ldoc::Vec2 size;
};

// ponytail: Scene collections only; add OBS profile export when output settings are requested.
struct Exporter {
    const ldoc::Project& project;
    fs::path directory;
    J root;
    std::map<ldoc::Id, Record> records;
    std::set<std::string> names;
    std::vector<std::string> warnings;

    Record record(const std::string& label, ldoc::Vec2 size) {
        std::string name = label.empty() ? "Unnamed" : label;
        for (int i = 2; !names.insert(name).second; ++i)
            name = label + " (" + std::to_string(i) + ")";
        return {name, uuid(), size};
    }

    void warn(const std::string& name, const std::string& text) {
        warnings.push_back(name + ": " + text);
    }

    std::string path(const std::string& value) const {
        if (value.empty())
            return value;
        const QUrl url(QString::fromStdString(value));
        if (url.isLocalFile())
            return url.toLocalFile().toStdString();
        if (url.scheme().size() > 1)
            return value;
        const fs::path p(value);
        return (p.is_absolute() ? p : directory / p).lexically_normal().string();
    }

    J base(const Record& rec, const std::string& type) const {
        J out;
        out["name"] = rec.name;
        out["uuid"] = rec.uuid;
        out["id"] = type;
        out["versioned_id"] = type;
        out["settings"] = J(Json::objectValue);
        out["volume"] = 1.0;
        out["enabled"] = true;
        out["mixers"] = 1;
        return out;
    }

    ldoc::Vec2 canvasSize(const ldoc::Id& id) const {
        for (const auto& canvas : project.canvases)
            if (canvas.id == id)
                return {static_cast<double>(canvas.width), static_cast<double>(canvas.height)};
        return {1920, 1080};
    }

    J source(const ldoc::Source& src) {
        auto& rec = records.at(src.id);
        const auto text = [&](const char* key, std::string fallback = {}) {
            return setting(src, key, std::move(fallback));
        };
        const auto number = [&](const char* key, std::int64_t fallback) {
            return setting(src, key, fallback);
        };
        const auto flag = [&](const char* key, bool fallback) {
            return setting(src, key, fallback);
        };
        std::string type;
        J settings(Json::objectValue);
        if (src.type == "source.color") {
            type = "color_source_v3";
            const auto color = setting(src, "color", ldoc::Rgba{}).to_srgb8();
            settings["color"] = Json::UInt(
                static_cast<unsigned>(color[0]) | (static_cast<unsigned>(color[1]) << 8) |
                (static_cast<unsigned>(color[2]) << 16) | (static_cast<unsigned>(color[3]) << 24));
        } else if (src.type == "source.image") {
            type = "image_source";
            settings["file"] = path(text("file"));
            settings["unload"] = flag("unloadWhenHidden", false);
            settings["linear_alpha"] = flag("alphaLinear", false);
        } else if (src.type == "source.text") {
            type = "text_ft2_source_v2";
            settings["text"] = text("text");
            settings["font"]["face"] = text("font", "Sans");
            settings["font"]["size"] = Json::Int64(number("size", 32));
            const auto c = setting(src, "color", ldoc::Rgba{1, 1, 1, 1}).to_srgb8();
            settings["color1"] = Json::UInt(
                static_cast<unsigned>(c[0]) | (static_cast<unsigned>(c[1]) << 8) |
                (static_cast<unsigned>(c[2]) << 16) | (static_cast<unsigned>(c[3]) << 24));
            settings["color2"] = settings["color1"];
            warn(src.name, "review font metrics, alignment and wrapping in OBS");
        } else if (src.type == "source.browser") {
            type = "browser_source";
            settings["url"] = text("url");
            settings["css"] = text("css");
            settings["fps"] = Json::Int64(number("fps", 30));
            settings["shutdown"] = true;
            warn(src.name, "browser session state and audio monitoring require review");
        } else if (src.type == "source.media") {
            type = "ffmpeg_source";
            const auto file = path(text("localFile"));
            const auto url = QUrl(QString::fromStdString(file));
            const bool local = url.scheme().isEmpty() || url.isLocalFile();
            settings["is_local_file"] = local;
            settings[local ? "local_file" : "input"] =
                local && url.isLocalFile() ? url.toLocalFile().toStdString() : file;
            settings["looping"] = flag("loop", false);
            settings["clear_on_media_end"] = flag("clearOnEnd", true);
            settings["speed_percent"] = Json::Int64(number("speedPercent", 100));
            warn(src.name, "review media dimensions, playback and audio in OBS");
        } else if (src.type == "source.playlist" || src.type == "source.slideshow") {
            const bool slides = src.type == "source.slideshow";
            type = slides ? "slideshow" : "vlc_source";
            settings[slides ? "files" : "playlist"] = J(Json::arrayValue);
            std::istringstream files(text("files"));
            std::string file;
            while (std::getline(files, file))
                if (!file.empty()) {
                    J entry;
                    entry["value"] = path(file);
                    entry["hidden"] = false;
                    entry["selected"] = false;
                    settings[slides ? "files" : "playlist"].append(entry);
                }
            settings["loop"] = flag("loop", true);
            settings["slide_time"] = Json::Int64(number("intervalMs", 8000));
            settings["transition"] = "cut";
            warn(src.name,
                 "review playback size and timing in OBS; VLC plugin needed for playlists");
        } else if (src.type == "source.audio-input" || src.type == "source.audio-output") {
            type =
                src.type == "source.audio-input" ? "pulse_input_capture" : "pulse_output_capture";
            settings["device_id"] = text("device", "default");
        } else if (src.type == "source.v4l2") {
            if (ldoc::has_audio(src))
                warn(src.name,
                     "attached capture audio requires a separate audio input source in OBS");
            type = "v4l2_input";
            settings["device_id"] = text("device");
            settings["buffering"] = flag("buffering", true);
            settings["auto_reset"] = flag("autoReset", false);
            settings["timeout_frames"] = Json::Int64(number("timeoutFrames", 5));
            const auto format = text("videoFormat", "Auto");
            std::uint32_t code = 0;
            if (format.size() == 4)
                for (unsigned i = 0; i < 4; ++i)
                    code |= static_cast<std::uint32_t>(static_cast<unsigned char>(format[i]))
                            << (8 * i);
            settings["pixelformat"] = format.size() == 4 ? Json::Int64(code) : Json::Int64(-1);
            const auto resolution = text("resolution");
            int width = 0, height = 0;
            if (std::sscanf(resolution.c_str(), "%dx%d", &width, &height) == 2 && width > 0 &&
                height > 0)
                settings["resolution"] =
                    Json::Int64((static_cast<std::int64_t>(width) << 32) | height);
            else
                settings["resolution"] = Json::Int64(-1);
            const auto rate = text("frameRate", "Leave unchanged");
            if (rate != "Leave unchanged") {
                const double fps = std::stod(rate);
                const int num = rate == "59.94"   ? 60000
                                : rate == "29.97" ? 30000
                                                  : static_cast<int>(fps);
                const int den = rate == "59.94" || rate == "29.97" ? 1001 : 1;
                settings["framerate"] = Json::Int64((static_cast<std::int64_t>(den) << 32) | num);
            } else
                settings["framerate"] = Json::Int64(-1);
            warn(src.name, "verify video mode, colour range and device lifetime in OBS");
        } else if (src.type == "source.screen" || src.type == "source.window" ||
                   src.type == "source.game") {
            type = src.type == "source.screen" ? "pipewire-screen-capture-source"
                                               : "pipewire-window-capture-source";
            settings["ShowCursor"] = flag("showCursor", true);
            warn(src.name, "select capture target again through OBS's Wayland portal; game capture "
                           "uses window capture");
        } else if (src.type == "source.obs-unsupported") {
            std::istringstream input(text("obs.original"));
            Json::CharReaderBuilder reader;
            reader["rejectDupKeys"] = true;
            J original;
            std::string error;
            if (!Json::parseFromStream(reader, input, &original, &error) || !original.isObject() ||
                !original["id"].isString())
                throw std::runtime_error(src.name + ": retained OBS record is invalid");
            original["name"] = rec.name;
            original["uuid"] = rec.uuid;
            warn(
                src.name,
                "original unsupported OBS plugin record restored; plugin must be installed in OBS");
            return original;
        } else {
            type = "color_source_v3";
            settings["color"] = Json::UInt(0);
            warn(src.name, "unsupported " + src.type +
                               " exported as transparent placeholder; replace it in OBS");
        }
        settings["width"] = static_cast<int>(rec.size.x);
        settings["height"] = static_cast<int>(rec.size.y);
        auto out = base(rec, type);
        // OBS's unversioned IDs differ from their current implementation IDs.
        if (type == "color_source_v3")
            out["id"] = "color_source";
        if (type == "text_ft2_source_v2")
            out["id"] = "text_ft2_source";
        out["settings"] = settings;
        return out;
    }

    J items(const std::vector<ldoc::Node>& nodes, ldoc::Vec2 canvas, int depth = 0) {
        if (depth > 64)
            throw std::runtime_error("scene nesting exceeds export limit");
        J result(Json::arrayValue);
        int index = 0;
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            const auto& node = *it;
            Record rec;
            if (const auto* src = std::get_if<ldoc::SourceData>(&node.data);
                src && src->source && records.contains(*src->source)) {
                rec = records.at(*src->source);
                if (src->audio.mute || src->audio.gain_db != 0 || src->audio.bus ||
                    !src->audio.sends.empty())
                    warn(node.name,
                         "per-placement audio routing/gain/mute omitted; configure OBS mixer");
            } else if (const auto* scene = std::get_if<ldoc::PrecompData>(&node.data);
                       scene && records.contains(scene->scene)) {
                rec = records.at(scene->scene);
                if (scene->size.x > 0 && scene->size.y > 0 && scene->size != rec.size)
                    warn(node.name,
                         "custom nested-scene size differs from scene canvas; check scaling");
            } else if (const auto* group = node.children()) {
                rec = record(node.name, canvas);
                auto nested = base(rec, "scene");
                nested["settings"]["custom_size"] = true;
                nested["settings"]["cx"] = static_cast<int>(canvas.x);
                nested["settings"]["cy"] = static_cast<int>(canvas.y);
                nested["settings"]["items"] = items(*group, canvas, depth + 1);
                root["sources"].append(nested);
                warn(node.name, "group exported as a nested OBS scene");
            } else {
                warn(node.name, "unsupported or unbound node omitted");
                continue;
            }
            lcomp::GapReason why;
            const auto frame = lcomp::drawn_frame(node, rec.size.x, rec.size.y, true, why);
            if (!frame || frame->width <= 0 || frame->height <= 0) {
                warn(node.name, "unsupported transform or empty crop omitted");
                continue;
            }
            const double sx = node.transform.scale.x, sy = node.transform.scale.y;
            const double dx = (frame->rel_dx - frame->rel_cx) * sx;
            const double dy = (frame->rel_dy - frame->rel_cy) * sy;
            const double angle = node.transform.rotation_deg * std::acos(-1.0) / 180;
            J item;
            item["name"] = rec.name;
            item["source_uuid"] = rec.uuid;
            item["id"] = ++index;
            item["visible"] = node.visible;
            item["locked"] = node.locked;
            item["align"] = 5;
            item["bounds_type"] = 0;
            item["rot"] = node.transform.rotation_deg;
            item["pos"] = vector(node.transform.position.x + frame->rel_cx + dx * std::cos(angle) -
                                     dy * std::sin(angle),
                                 node.transform.position.y + frame->rel_cy + dx * std::sin(angle) +
                                     dy * std::cos(angle));
            item["scale"] =
                vector(sx * frame->base_width /
                           (rec.size.x - node.layout.crop.left - node.layout.crop.right),
                       sy * frame->base_height /
                           (rec.size.y - node.layout.crop.top - node.layout.crop.bottom));
            const auto crop = [&](const char* key, double edge) {
                item[key] = static_cast<int>(std::round(edge));
                if (std::round(edge) != edge)
                    warn(node.name, "fractional crop rounded to OBS integer pixels");
            };
            crop("crop_left", node.layout.crop.left);
            crop("crop_top", node.layout.crop.top);
            crop("crop_right", node.layout.crop.right);
            crop("crop_bottom", node.layout.crop.bottom);
            item["blend_type"] = "normal";
            if (lcomp::blend_supported(node.blend.mode)) {
                item["blend_type"] = node.blend.mode == ldoc::BlendMode::LinearDodge
                                         ? "additive"
                                         : std::string(ldoc::to_string(node.blend.mode));
                if (node.blend.mode != ldoc::BlendMode::Normal)
                    warn(node.name,
                         "blend mode converted; OBS alpha may differ for transparent layers");
            } else {
                warn(node.name, "unsupported blend mode; uses normal blend");
            }
            if (node.blend.opacity != 1 || node.blend.clip_to_below || !node.effects.empty())
                warn(node.name, "opacity, clipping and effects not converted; review OBS filters");
            result.append(item);
        }
        return result;
    }

    J convert() {
        root["name"] = project.name;
        root["sources"] = J(Json::arrayValue);
        root["groups"] = J(Json::arrayValue);
        root["scene_order"] = J(Json::arrayValue);
        for (const auto& scene : project.scenes)
            records.emplace(scene.id, record(scene.name, canvasSize(scene.canvas)));
        for (const auto& src : project.sources) {
            const auto dimension = [&](const char* key, std::int64_t fallback) {
                const double value =
                    setting(src, key, static_cast<double>(setting(src, key, fallback)));
                if (!std::isfinite(value) || value < 1 || value > 8192)
                    throw std::runtime_error(src.name + ": invalid source dimension");
                return value;
            };
            ldoc::Vec2 size{dimension("width", 1920), dimension("height", 1080)};
            if (src.type == "source.image") {
                const auto file = path(setting(src, "file", std::string{}));
                const auto measured = QImageReader(QString::fromStdString(file)).size();
                if (measured.isValid())
                    size = {static_cast<double>(measured.width()),
                            static_cast<double>(measured.height())};
            }
            if (size.x <= 0 || size.y <= 0)
                size = {1920, 1080};
            records.emplace(src.id, record(src.name, size));
            root["sources"].append(source(src));
        }
        for (const auto& scene : project.scenes) {
            const auto& rec = records.at(scene.id);
            auto out = base(rec, "scene");
            out["settings"]["custom_size"] = true;
            out["settings"]["cx"] = static_cast<int>(rec.size.x);
            out["settings"]["cy"] = static_cast<int>(rec.size.y);
            out["settings"]["items"] = items(scene.roots, rec.size);
            root["sources"].append(out);
            J order;
            order["name"] = rec.name;
            root["scene_order"].append(order);
        }
        if (!project.scenes.empty()) {
            auto current = project.scenes.front().id;
            for (const auto& deck : project.decks)
                if (!project.canvases.empty() && deck.canvas == project.canvases.front().id &&
                    deck.program_scene)
                    current = *deck.program_scene;
            if (records.contains(current)) {
                root["current_scene"] = records.at(current).name;
                root["current_program_scene"] = records.at(current).name;
            }
        }
        return root;
    }
};

std::expected<void, std::string> writeNew(const fs::path& path, const std::string& bytes) {
    const auto temporary = path.string() + "." + uuid() + ".tmp";
    auto written = lc::write_atomic(temporary, bytes);
    if (!written)
        return written;
    const int result = ::link(temporary.c_str(), path.c_str());
    const auto why = std::error_code(errno, std::generic_category()).message();
    std::error_code ec;
    fs::remove(temporary, ec);
    if (result != 0)
        return std::unexpected("Cannot create " + path.string() + ": " + why);
    const int fd = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return std::unexpected("Cannot sync export directory.");
    const int synced = ::fsync(fd);
    ::close(fd);
    if (synced != 0)
        return std::unexpected("Cannot sync export directory.");
    return {};
}
} // namespace

std::expected<std::string, std::string> export_obs(const ldoc::Project& project,
                                                   const fs::path& project_directory,
                                                   const fs::path& destination) {
    try {
        const auto reportPath = fs::path(destination.string() + ".report.md");
        if (destination.empty() || !destination.is_absolute())
            return std::unexpected("Choose an absolute export path.");
        if (fs::exists(destination) || fs::exists(reportPath))
            return std::unexpected("Export or report path already exists.");
        Exporter exporter{project, project_directory, {}, {}, {}, {}};
        const auto root = exporter.convert();
        std::string report = "# OBS Export Report\n\nCollection: " + project.name + "\n\n";
        if (!project.canvases.empty()) {
            const auto& c = project.canvases.front();
            report += "Set OBS profile canvas to " + std::to_string(c.width) + "x" +
                      std::to_string(c.height) + " at " + std::to_string(c.fps.num) + "/" +
                      std::to_string(c.fps.den) + " fps.\n\n";
        }
        report +=
            "Scene collection JSON does not carry Libertas outputs, credentials, encoder settings, "
            "bus routing, hotkeys, favourites, emergency scenes or canvas links. Review OBS audio "
            "mixer, "
            "devices and media paths. External media is referenced, not embedded.\n";
        if (project.canvases.size() > 1)
            report += "\nExtra canvases become custom-size scenes; configure vertical output "
                      "separately in OBS.\n";
        for (const auto& warning : exporter.warnings)
            report += "\n- " + warning;
        report += "\n";
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        auto saved = writeNew(reportPath, report);
        if (!saved)
            return std::unexpected(saved.error());
        saved = writeNew(destination, Json::writeString(writer, root) + "\n");
        if (!saved) {
            std::error_code ec;
            fs::remove(reportPath, ec);
            return std::unexpected(saved.error());
        }
        return report;
    } catch (const std::exception& error) {
        return std::unexpected(error.what());
    }
}
} // namespace limport
