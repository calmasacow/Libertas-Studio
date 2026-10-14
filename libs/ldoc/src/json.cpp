// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/json.hpp"

#include "json_detail.hpp"

#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>

namespace ldoc {
namespace detail {

using Jv = Json::Value;

// Declared up front because `enc_array` is a template: at its point of instantiation only the
// declarations visible at its definition are considered, and argument-dependent lookup does not
// reach into this unnamed namespace.
Jv enc(const Send& s);
Jv enc(const Effect& e);
Jv enc(const Canvas& c);
Jv enc(const Bus& b);
Jv enc(const Deck& d);
Jv enc(const Source& s);
Jv enc(const Asset& a);
Jv enc(const Output& o);

// ---- writing --------------------------------------------------------------------------------

Jv enc(const Id& id) { return Jv{id.to_string()}; }

Jv enc(const Vec2& v) {
    Jv o{Json::objectValue};
    o["x"] = v.x;
    o["y"] = v.y;
    return o;
}

Jv enc(const Rgba& c) {
    Jv o{Json::objectValue};
    o["r"] = c.r;
    o["g"] = c.g;
    o["b"] = c.b;
    o["a"] = c.a;
    return o;
}

Jv enc(const Fps& f) {
    Jv o{Json::objectValue};
    o["num"] = f.num;
    o["den"] = f.den;
    return o;
}

/// A ULID is wrapped as `{"id": "..."}` rather than written bare.
///
/// A bare 26-character string is indistinguishable from a string parameter that happens to look
/// like one, so a project with a text field containing an id-shaped value would not survive a
/// round trip. The wrapper costs eight characters and removes the ambiguity entirely.
Jv enc_param_id(const Id& id) {
    Jv o{Json::objectValue};
    o["id"] = id.to_string();
    return o;
}

/// Whole numbers are tagged the same way, for the same reason.
///
/// JSON has one number type, and jsoncpp writes a double of 3.0 as `3` -- which parses back as an
/// integer. A parameter declared as a float in an effect manifest would silently become an
/// integer on the first save and stay one. The rule here is therefore: a bare JSON number is
/// always a double, and anything else carries a tag. Floats are overwhelmingly the common case in
/// parameters, so the untagged form is the readable one.
Jv enc_param_int(std::int64_t v) {
    Jv o{Json::objectValue};
    o["int"] = static_cast<Json::Int64>(v);
    return o;
}

Jv enc(const ParamValue& v) {
    return std::visit(
        [](const auto& x) -> Jv {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return Jv{Json::nullValue};
            else if constexpr (std::is_same_v<T, bool>)
                return Jv{x};
            else if constexpr (std::is_same_v<T, std::int64_t>)
                return enc_param_int(x);
            else if constexpr (std::is_same_v<T, double>)
                return Jv{x};
            else if constexpr (std::is_same_v<T, std::string>)
                return Jv{x};
            else if constexpr (std::is_same_v<T, Vec2>)
                return enc(x);
            else if constexpr (std::is_same_v<T, Rgba>)
                return enc(x);
            else
                return enc_param_id(x);
        },
        v);
}

Jv enc(const ParamMap& m) {
    Jv o{Json::objectValue};
    for (const auto& [k, v] : m)
        o[k] = enc(v);
    return o;
}

Jv enc(const Transform& t) {
    Jv o{Json::objectValue};
    o["position"] = enc(t.position);
    o["rotation_deg"] = t.rotation_deg;
    o["scale"] = enc(t.scale);
    o["pivot"] = enc(t.pivot);
    o["skew_deg"] = enc(t.skew_deg);
    return o;
}

Jv enc(const CropRect& c) {
    Jv o{Json::objectValue};
    o["left"] = c.left;
    o["top"] = c.top;
    o["right"] = c.right;
    o["bottom"] = c.bottom;
    return o;
}

Jv enc(const Layout& l) {
    Jv o{Json::objectValue};
    o["fit"] = std::string{to_string(l.fit)};
    o["crop"] = enc(l.crop);
    o["size"] = enc(l.size);
    o["alignment"] = enc(l.alignment);
    return o;
}

Jv enc(const Blend& b) {
    Jv o{Json::objectValue};
    o["mode"] = std::string{to_string(b.mode)};
    o["opacity"] = b.opacity;
    o["clip_to_below"] = b.clip_to_below;
    o["pass_through_group"] = b.pass_through_group;
    return o;
}

Jv enc(const Effect& e) {
    Jv o{Json::objectValue};
    o["id"] = enc(e.id);
    o["type"] = e.type;
    o["enabled"] = e.enabled;
    o["params"] = enc(e.params);
    o["mask"] = e.mask ? enc(*e.mask) : Jv{Json::nullValue};
    o["blend"] = e.blend ? enc(*e.blend) : Jv{Json::nullValue};
    return o;
}

template <typename T> Jv enc_array(const std::vector<T>& items) {
    Jv a{Json::arrayValue};
    for (const T& item : items)
        a.append(enc(item));
    return a;
}

Jv enc(const Send& s) {
    Jv o{Json::objectValue};
    o["to"] = enc(s.to);
    o["gain_db"] = s.gain_db;
    o["pre_fader"] = s.pre_fader;
    return o;
}

Jv enc(const AudioRouting& a) {
    Jv o{Json::objectValue};
    o["bus"] = a.bus ? enc(*a.bus) : Jv{Json::nullValue};
    o["sends"] = enc_array(a.sends);
    o["gain_db"] = a.gain_db;
    o["mute"] = a.mute;
    return o;
}

Jv enc(const CanvasOverride& ov) {
    Jv o{Json::objectValue};
    o["transform"] = ov.transform ? enc(*ov.transform) : Jv{Json::nullValue};
    o["layout"] = ov.layout ? enc(*ov.layout) : Jv{Json::nullValue};
    o["visible"] = ov.visible ? Jv{*ov.visible} : Jv{Json::nullValue};
    o["blend"] = ov.blend ? enc(*ov.blend) : Jv{Json::nullValue};
    return o;
}

Jv enc(const Node& n);

Jv enc_node_data(const Node& n) {
    Jv o{Json::objectValue};
    std::visit(
        [&o](const auto& d) {
            using T = std::decay_t<decltype(d)>;
            if constexpr (std::is_same_v<T, GroupData>) {
                Jv kids{Json::arrayValue};
                for (const Node& c : d.children)
                    kids.append(enc(c));
                o["children"] = kids;
                o["isolate"] = d.isolate;
            } else if constexpr (std::is_same_v<T, SourceData>) {
                o["source"] = d.source ? enc(*d.source) : Jv{Json::nullValue};
                o["audio"] = enc(d.audio);
            } else if constexpr (std::is_same_v<T, AdjustmentData>) {
                // Nothing of its own; the effect stack is the whole point of the kind.
            } else if constexpr (std::is_same_v<T, MaskData>) {
                o["mask_kind"] = std::string{to_string(d.kind)};
                o["invert"] = d.invert;
                o["shape"] = enc(d.shape);
            } else if constexpr (std::is_same_v<T, ReferenceData>) {
                o["target"] = enc(d.target);
                o["mode"] = std::string{to_string(d.mode)};
            } else {
                o["scene"] = enc(d.scene);
                o["size"] = enc(d.size);
                o["color_space"] = std::string{to_string(d.color_space)};
            }
        },
        n.data);
    return o;
}

Jv enc(const Node& n) {
    Jv o{Json::objectValue};
    o["id"] = enc(n.id);
    o["kind"] = std::string{to_string(n.kind())};
    o["name"] = n.name;
    o["visible"] = n.visible;
    o["locked"] = n.locked;
    o["transform"] = enc(n.transform);
    o["layout"] = enc(n.layout);
    o["blend"] = enc(n.blend);
    o["effects"] = enc_array(n.effects);

    Jv overrides{Json::objectValue};
    for (const auto& [canvas_id, ov] : n.canvas_overrides)
        overrides[canvas_id.to_string()] = enc(ov);
    o["canvas_overrides"] = overrides;

    o["data"] = enc_node_data(n);
    return o;
}

Jv enc(const Canvas& c) {
    Jv o{Json::objectValue};
    o["id"] = enc(c.id);
    o["name"] = c.name;
    o["width"] = c.width;
    o["height"] = c.height;
    o["fps"] = enc(c.fps);
    o["color_space"] = std::string{to_string(c.color_space)};
    o["emergency_scene"] = c.emergency_scene ? enc(*c.emergency_scene) : Jv{Json::nullValue};
    return o;
}

Jv enc(const Bus& b) {
    Jv o{Json::objectValue};
    o["id"] = enc(b.id);
    o["name"] = b.name;
    o["kind"] = std::string{to_string(b.kind)};
    o["channels"] = b.channels;
    o["inserts"] = enc_array(b.inserts);
    o["sends"] = enc_array(b.sends);
    o["gain_db"] = b.gain_db;
    o["pan"] = b.pan;
    o["mute"] = b.mute;
    o["solo"] = b.solo;
    o["delay_ms"] = b.delay_ms;
    return o;
}

Jv enc(const Deck& d) {
    Jv o{Json::objectValue};
    o["id"] = enc(d.id);
    o["name"] = d.name;
    o["canvas"] = enc(d.canvas);
    o["program_scene"] = d.program_scene ? enc(*d.program_scene) : Jv{Json::nullValue};
    o["preview_scene"] = d.preview_scene ? enc(*d.preview_scene) : Jv{Json::nullValue};
    o["studio_mode"] = d.studio_mode;
    o["transition"] = d.transition;
    o["transition_ms"] = d.transition_ms;
    return o;
}

Jv enc(const Source& s) {
    Jv o{Json::objectValue};
    o["id"] = enc(s.id);
    o["name"] = s.name;
    o["type"] = s.type;
    o["settings"] = enc(s.settings);
    o["asset"] = s.asset ? enc(*s.asset) : Jv{Json::nullValue};
    return o;
}

Jv enc(const Asset& a) {
    Jv o{Json::objectValue};
    o["id"] = enc(a.id);
    o["path"] = a.path;
    o["sha256"] = a.sha256;
    o["bytes"] = static_cast<Json::UInt64>(a.bytes);
    Jv hints{Json::arrayValue};
    for (const std::string& h : a.relocation_hints)
        hints.append(h);
    o["relocation_hints"] = hints;
    return o;
}

Jv enc(const EncoderConfig& e) {
    Jv o{Json::objectValue};
    o["codec"] = e.codec;
    o["implementation"] = e.implementation;
    o["bitrate_kbps"] = e.bitrate_kbps;
    o["keyframe_interval_ms"] = e.keyframe_interval_ms;
    o["preset"] = e.preset;
    o["rate_control"] = e.rate_control;
    o["extra"] = enc(e.extra);
    return o;
}

Jv enc(const Destination& d) {
    Jv o{Json::objectValue};
    o["kind"] = std::string{to_string(d.kind)};
    o["url"] = d.url;
    // The *name* of a credential. See the comment on Destination: there is deliberately nowhere
    // here to put the secret itself.
    o["credential_ref"] = d.credential_ref;
    return o;
}

Jv enc(const Output& o_) {
    Jv o{Json::objectValue};
    o["id"] = enc(o_.id);
    o["name"] = o_.name;
    o["canvas"] = enc(o_.canvas);
    o["encoder"] = enc(o_.encoder);
    o["destination"] = enc(o_.destination);
    o["enabled"] = o_.enabled;
    return o;
}

Jv enc_scene(const Scene& scene) {
    Jv root{Json::objectValue};
    root["id"] = enc(scene.id);
    root["name"] = scene.name;
    root["favourite"] = scene.favourite;

    Jv roots{Json::arrayValue};
    for (const Node& n : scene.roots)
        roots.append(enc(n));
    root["roots"] = roots;

    Jv overrides{Json::objectValue};
    for (const auto& [node_id, ov] : scene.audio_overrides) {
        Jv o{Json::objectValue};
        o["bus"] = ov.bus ? enc(*ov.bus) : Jv{Json::nullValue};
        o["gain_db"] = ov.gain_db ? Jv{*ov.gain_db} : Jv{Json::nullValue};
        o["mute"] = ov.mute ? Jv{*ov.mute} : Jv{Json::nullValue};
        if (ov.sends) {
            Jv sends{Json::arrayValue};
            for (const Send& s : *ov.sends)
                sends.append(enc(s));
            o["sends"] = sends;
        } else {
            o["sends"] = Jv{Json::nullValue};
        }
        overrides[node_id.to_string()] = o;
    }
    root["audio_overrides"] = overrides;
    return root;
}

Jv enc(const Settings& s) {
    Jv o{Json::objectValue};
    o["sample_rate"] = s.sample_rate;
    o["render_queue_frames"] = s.render_queue_frames;
    o["extra"] = enc(s.extra);
    return o;
}

std::string write(const Jv& root) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "  ";
    // Enough digits that a double survives the round trip exactly.
    b["precision"] = 17;
    b["precisionType"] = "significant";
    b["emitUTF8"] = true;
    const std::unique_ptr<Json::StreamWriter> w{b.newStreamWriter()};
    std::ostringstream out;
    w->write(root, &out);
    out << '\n';
    return out.str();
}

} // namespace detail

using namespace detail;

std::string scene_to_json(const Scene& scene, std::uint64_t generation) {
    Jv root = detail::enc_scene(scene);
    root["schema_version"] = kSchemaVersion;
    root["generation"] = static_cast<Json::UInt64>(generation);
    return write(root);
}

std::string assets_to_json(const std::vector<Asset>& assets) {
    Jv root{Json::objectValue};
    root["schema_version"] = kSchemaVersion;
    root["assets"] = enc_array(assets);
    return write(root);
}

std::string project_to_json(const Project& project) {
    Jv root{Json::objectValue};
    root["schema_version"] = project.schema_version;
    root["id"] = enc(project.id);
    root["name"] = project.name;
    root["generation"] = static_cast<Json::UInt64>(project.generation);
    root["canvases"] = enc_array(project.canvases);
    root["buses"] = enc_array(project.buses);
    root["decks"] = enc_array(project.decks);
    root["sources"] = enc_array(project.sources);
    root["outputs"] = enc_array(project.outputs);
    root["settings"] = enc(project.settings);

    // The scene manifest. Each entry carries the generation that scene file was last written in,
    // which is how a torn save is detected on open: a scene whose file disagrees with this list
    // was written by a different save than project.json was.
    Jv scenes{Json::arrayValue};
    for (const Scene& s : project.scenes) {
        Jv e{Json::objectValue};
        e["id"] = enc(s.id);
        e["name"] = s.name;
        e["generation"] = static_cast<Json::UInt64>(project.generation);
        scenes.append(e);
    }
    root["scenes"] = scenes;
    return write(root);
}


// ---- reading --------------------------------------------------------------------------------

namespace detail {

void Rd::fail(std::string_view path, std::string_view msg) {
    if (err_.empty())
        err_ = std::format("{}: {}", path, msg);
}

/// Present but the wrong type is an error; absent takes the default. Lenient on read, strict on
/// write: a project from an older build must still open, but a corrupted or hand-broken field
/// must not be silently reinterpreted.
bool Rd::boolean(const Jv& o, const char* key, std::string_view path, bool def) {
    const Jv& v = o[key];
    if (v.isNull())
        return def;
    if (!v.isBool()) {
        fail(path, std::format("'{}' is not a boolean", key));
        return def;
    }
    return v.asBool();
}

double Rd::number(const Jv& o, const char* key, std::string_view path, double def) {
    const Jv& v = o[key];
    if (v.isNull())
        return def;
    if (!v.isNumeric()) {
        fail(path, std::format("'{}' is not a number", key));
        return def;
    }
    return v.asDouble();
}

std::int64_t Rd::integer(const Jv& o, const char* key, std::string_view path, std::int64_t def) {
    const Jv& v = o[key];
    if (v.isNull())
        return def;
    if (!v.isIntegral()) {
        fail(path, std::format("'{}' is not a whole number", key));
        return def;
    }
    return v.asInt64();
}

std::int32_t Rd::integer32(const Jv& o, const char* key, std::string_view path,
                           std::int32_t def) {
    const std::int64_t v = integer(o, key, path, def);
    if (v < std::numeric_limits<std::int32_t>::min() ||
        v > std::numeric_limits<std::int32_t>::max()) {
        fail(path, std::format("'{}' is out of range", key));
        return def;
    }
    return static_cast<std::int32_t>(v);
}

std::string Rd::text(const Jv& o, const char* key, std::string_view path, std::string def) {
    const Jv& v = o[key];
    if (v.isNull())
        return def;
    if (!v.isString()) {
        fail(path, std::format("'{}' is not a string", key));
        return def;
    }
    return v.asString();
}

std::optional<Id> Rd::parse_id(std::string_view value, std::string_view path, const char* key) {
    const auto parsed = Id::parse(value);
    if (!parsed)
        fail(path, std::format("'{}' is not an id", key));
    return parsed;
}

/// A required id. A missing or malformed one is an error: nothing in this format is addressable
/// without it.
Id Rd::id(const Jv& o, const char* key, std::string_view path) {
    const Jv& v = o[key];
    if (!v.isString()) {
        fail(path, std::format("'{}' is missing or is not an id", key));
        return Id::nil();
    }
    return parse_id(v.asString(), path, key).value_or(Id::nil());
}

std::optional<Id> Rd::id_opt(const Jv& o, const char* key, std::string_view path) {
    const Jv& v = o[key];
    if (v.isNull())
        return std::nullopt;
    if (!v.isString()) {
        fail(path, std::format("'{}' is not an id", key));
        return std::nullopt;
    }
    return parse_id(v.asString(), path, key);
}

const Jv* Rd::object(const Jv& o, const char* key, std::string_view path) {
    const Jv& v = o[key];
    if (v.isNull())
        return nullptr;
    if (!v.isObject()) {
        fail(path, std::format("'{}' is not an object", key));
        return nullptr;
    }
    return &v;
}

const Jv* Rd::array(const Jv& o, const char* key, std::string_view path) {
    const Jv& v = o[key];
    if (v.isNull())
        return nullptr;
    if (!v.isArray()) {
        fail(path, std::format("'{}' is not an array", key));
        return nullptr;
    }
    return &v;
}

Vec2 dec_vec2(Rd& rd, const Jv& o, std::string_view path) {
    return Vec2{rd.number(o, "x", path, 0.0), rd.number(o, "y", path, 0.0)};
}

Vec2 dec_vec2_at(Rd& rd, const Jv& parent, const char* key, std::string_view path, Vec2 def) {
    const Jv* o = rd.object(parent, key, path);
    return o ? dec_vec2(rd, *o, path) : def;
}

Rgba dec_rgba(Rd& rd, const Jv& o, std::string_view path) {
    return Rgba{rd.number(o, "r", path, 0.0), rd.number(o, "g", path, 0.0),
                rd.number(o, "b", path, 0.0), rd.number(o, "a", path, 1.0)};
}

ParamValue dec_param(Rd& rd, const Jv& v, std::string_view path) {
    if (v.isNull())
        return std::monostate{};
    if (v.isBool())
        return v.asBool();
    // A bare number is always a double; see enc_param_int.
    if (v.isNumeric())
        return v.asDouble();
    if (v.isString())
        return v.asString();
    if (v.isObject()) {
        if (v.isMember("int")) {
            const Jv& n = v["int"];
            if (!n.isIntegral()) {
                rd.fail(path, "an integer parameter is not a whole number");
                return std::monostate{};
            }
            return static_cast<std::int64_t>(n.asInt64());
        }
        if (v.isMember("id")) {
            const Jv& idv = v["id"];
            if (!idv.isString()) {
                rd.fail(path, "an id parameter is not a string");
                return std::monostate{};
            }
            const auto id = Id::parse(idv.asString());
            if (!id) {
                rd.fail(path, "an id parameter is not an id");
                return std::monostate{};
            }
            return *id;
        }
        if (v.isMember("r"))
            return dec_rgba(rd, v, path);
        if (v.isMember("x"))
            return dec_vec2(rd, v, path);
    }
    rd.fail(path, "a parameter has a type this format does not define");
    return std::monostate{};
}

ParamMap dec_params(Rd& rd, const Jv& parent, const char* key, std::string_view path) {
    ParamMap out;
    const Jv* o = rd.object(parent, key, path);
    if (!o)
        return out;
    for (const std::string& name : o->getMemberNames())
        out.emplace(name, dec_param(rd, (*o)[name], path));
    return out;
}

Transform dec_transform(Rd& rd, const Jv& parent, const char* key, std::string_view path) {
    Transform t;
    const Jv* o = rd.object(parent, key, path);
    if (!o)
        return t;
    t.position = dec_vec2_at(rd, *o, "position", path, t.position);
    t.rotation_deg = rd.number(*o, "rotation_deg", path, t.rotation_deg);
    t.scale = dec_vec2_at(rd, *o, "scale", path, t.scale);
    t.pivot = dec_vec2_at(rd, *o, "pivot", path, t.pivot);
    t.skew_deg = dec_vec2_at(rd, *o, "skew_deg", path, t.skew_deg);
    return t;
}

Layout dec_layout(Rd& rd, const Jv& parent, const char* key, std::string_view path) {
    Layout l;
    const Jv* o = rd.object(parent, key, path);
    if (!o)
        return l;
    l.fit = rd.enumerated(*o, "fit", path, fit_mode_from_string, l.fit);
    if (const Jv* c = rd.object(*o, "crop", path)) {
        l.crop.left = rd.number(*c, "left", path, 0.0);
        l.crop.top = rd.number(*c, "top", path, 0.0);
        l.crop.right = rd.number(*c, "right", path, 0.0);
        l.crop.bottom = rd.number(*c, "bottom", path, 0.0);
    }
    l.size = dec_vec2_at(rd, *o, "size", path, l.size);
    l.alignment = dec_vec2_at(rd, *o, "alignment", path, l.alignment);
    return l;
}

Blend dec_blend_obj(Rd& rd, const Jv& o, std::string_view path) {
    Blend b;
    b.mode = rd.enumerated(o, "mode", path, blend_mode_from_string, b.mode);
    b.opacity = rd.number(o, "opacity", path, b.opacity);
    b.clip_to_below = rd.boolean(o, "clip_to_below", path, b.clip_to_below);
    b.pass_through_group = rd.boolean(o, "pass_through_group", path, b.pass_through_group);
    return b;
}

Blend dec_blend(Rd& rd, const Jv& parent, const char* key, std::string_view path) {
    const Jv* o = rd.object(parent, key, path);
    return o ? dec_blend_obj(rd, *o, path) : Blend{};
}

Effect dec_effect(Rd& rd, const Jv& o, std::string_view path) {
    Effect e;
    e.id = rd.id(o, "id", path);
    e.type = rd.text(o, "type", path);
    e.enabled = rd.boolean(o, "enabled", path, true);
    e.params = dec_params(rd, o, "params", path);
    e.mask = rd.id_opt(o, "mask", path);
    if (const Jv* b = rd.object(o, "blend", path))
        e.blend = dec_blend_obj(rd, *b, path);
    return e;
}

std::vector<Effect> dec_effects(Rd& rd, const Jv& parent, const char* key,
                                std::string_view path) {
    std::vector<Effect> out;
    const Jv* a = rd.array(parent, key, path);
    if (!a)
        return out;
    for (const Jv& v : *a) {
        if (!v.isObject()) {
            rd.fail(path, "an effect is not an object");
            return out;
        }
        out.push_back(dec_effect(rd, v, path));
    }
    return out;
}

Send dec_send(Rd& rd, const Jv& o, std::string_view path) {
    Send s;
    s.to = rd.id(o, "to", path);
    s.gain_db = rd.number(o, "gain_db", path, 0.0);
    s.pre_fader = rd.boolean(o, "pre_fader", path, false);
    return s;
}

std::vector<Send> dec_sends(Rd& rd, const Jv& parent, const char* key, std::string_view path) {
    std::vector<Send> out;
    const Jv* a = rd.array(parent, key, path);
    if (!a)
        return out;
    for (const Jv& v : *a) {
        if (!v.isObject()) {
            rd.fail(path, "a send is not an object");
            return out;
        }
        out.push_back(dec_send(rd, v, path));
    }
    return out;
}

AudioRouting dec_audio_routing(Rd& rd, const Jv& o, std::string_view path) {
    AudioRouting a;
    a.bus = rd.id_opt(o, "bus", path);
    a.sends = dec_sends(rd, o, "sends", path);
    a.gain_db = rd.number(o, "gain_db", path, 0.0);
    a.mute = rd.boolean(o, "mute", path, false);
    return a;
}

Node dec_node(Rd& rd, const Jv& o, std::string_view path);

CanvasOverride dec_canvas_override(Rd& rd, const Jv& e, std::string_view path) {
    CanvasOverride co;
    if (rd.object(e, "transform", path))
        co.transform = dec_transform(rd, e, "transform", path);
    if (rd.object(e, "layout", path))
        co.layout = dec_layout(rd, e, "layout", path);
    if (!e["visible"].isNull())
        co.visible = rd.boolean(e, "visible", path, true);
    if (const Jv* b = rd.object(e, "blend", path))
        co.blend = dec_blend_obj(rd, *b, path);
    return co;
}

NodeData dec_node_data(Rd& rd, NodeKind kind, const Jv& parent, std::string_view path) {
    const Jv* o = rd.object(parent, "data", path);
    const Jv empty{Json::objectValue};
    const Jv& d = o ? *o : empty;

    switch (kind) {
    case NodeKind::Group: {
        GroupData g;
        g.isolate = rd.boolean(d, "isolate", path, false);
        if (const Jv* kids = rd.array(d, "children", path)) {
            for (const Jv& c : *kids) {
                if (!c.isObject()) {
                    rd.fail(path, "a child is not an object");
                    break;
                }
                g.children.push_back(dec_node(rd, c, path));
            }
        }
        return g;
    }
    case NodeKind::Source: {
        SourceData s;
        s.source = rd.id_opt(d, "source", path);
        if (const Jv* a = rd.object(d, "audio", path))
            s.audio = dec_audio_routing(rd, *a, path);
        return s;
    }
    case NodeKind::Adjustment:
        return AdjustmentData{};
    case NodeKind::Mask: {
        MaskData m;
        m.kind = rd.enumerated(d, "mask_kind", path, mask_kind_from_string, m.kind);
        m.invert = rd.boolean(d, "invert", path, false);
        m.shape = dec_params(rd, d, "shape", path);
        return m;
    }
    case NodeKind::Reference: {
        ReferenceData r;
        r.target = rd.id(d, "target", path);
        r.mode = rd.enumerated(d, "mode", path, reference_mode_from_string, r.mode);
        return r;
    }
    case NodeKind::Precomp: {
        PrecompData p;
        p.scene = rd.id(d, "scene", path);
        p.size = dec_vec2_at(rd, d, "size", path, p.size);
        p.color_space = rd.enumerated(d, "color_space", path, color_space_from_string,
                                      p.color_space);
        return p;
    }
    }
    return GroupData{};
}

Node dec_node(Rd& rd, const Jv& o, std::string_view path) {
    Node n;
    n.id = rd.id(o, "id", path);
    const NodeKind kind =
        rd.enumerated(o, "kind", path, node_kind_from_string, NodeKind::Group);
    n.name = rd.text(o, "name", path);
    n.visible = rd.boolean(o, "visible", path, true);
    n.locked = rd.boolean(o, "locked", path, false);
    n.transform = dec_transform(rd, o, "transform", path);
    n.layout = dec_layout(rd, o, "layout", path);
    n.blend = dec_blend(rd, o, "blend", path);
    n.effects = dec_effects(rd, o, "effects", path);

    if (const Jv* ov = rd.object(o, "canvas_overrides", path)) {
        for (const std::string& key : ov->getMemberNames()) {
            const auto canvas_id = Id::parse(key);
            if (!canvas_id) {
                rd.fail(path, "a canvas override is keyed by something that is not an id");
                break;
            }
            const Jv& e = (*ov)[key];
            if (!e.isObject()) {
                rd.fail(path, "a canvas override is not an object");
                break;
            }
            n.canvas_overrides.emplace(*canvas_id, dec_canvas_override(rd, e, path));
        }
    }

    n.data = dec_node_data(rd, kind, o, path);
    return n;
}

Canvas dec_canvas(Rd& rd, const Jv& v, std::string_view path) {
    Canvas c;
    c.id = rd.id(v, "id", path);
    c.name = rd.text(v, "name", path);
    c.width = rd.integer32(v, "width", path, c.width);
    c.height = rd.integer32(v, "height", path, c.height);
    if (const Jv* f = rd.object(v, "fps", path)) {
        c.fps.num = rd.integer32(*f, "num", path, c.fps.num);
        c.fps.den = rd.integer32(*f, "den", path, c.fps.den);
    }
    c.color_space = rd.enumerated(v, "color_space", path, color_space_from_string,
                                  c.color_space);
    c.emergency_scene = rd.id_opt(v, "emergency_scene", path);
    return c;
}

Bus dec_bus(Rd& rd, const Jv& v, std::string_view path) {
    Bus b;
    b.id = rd.id(v, "id", path);
    b.name = rd.text(v, "name", path);
    b.kind = rd.enumerated(v, "kind", path, bus_kind_from_string, b.kind);
    b.channels = rd.integer32(v, "channels", path, b.channels);
    b.inserts = dec_effects(rd, v, "inserts", path);
    b.sends = dec_sends(rd, v, "sends", path);
    b.gain_db = rd.number(v, "gain_db", path, 0.0);
    b.pan = rd.number(v, "pan", path, 0.0);
    b.mute = rd.boolean(v, "mute", path, false);
    b.solo = rd.boolean(v, "solo", path, false);
    b.delay_ms = rd.number(v, "delay_ms", path, 0.0);
    return b;
}

Deck dec_deck(Rd& rd, const Jv& v, std::string_view path) {
    Deck d;
    d.id = rd.id(v, "id", path);
    d.name = rd.text(v, "name", path);
    d.canvas = rd.id(v, "canvas", path);
    d.program_scene = rd.id_opt(v, "program_scene", path);
    d.preview_scene = rd.id_opt(v, "preview_scene", path);
    d.studio_mode = rd.boolean(v, "studio_mode", path, false);
    d.transition = rd.text(v, "transition", path, d.transition);
    d.transition_ms = rd.integer32(v, "transition_ms", path, d.transition_ms);
    return d;
}

Output dec_output(Rd& rd, const Jv& v, std::string_view path) {
    Output o;
    o.id = rd.id(v, "id", path);
    o.name = rd.text(v, "name", path);
    o.canvas = rd.id(v, "canvas", path);
    if (const Jv* e = rd.object(v, "encoder", path)) {
        o.encoder.codec = rd.text(*e, "codec", path, o.encoder.codec);
        o.encoder.implementation = rd.text(*e, "implementation", path,
                                           o.encoder.implementation);
        o.encoder.bitrate_kbps = rd.integer32(*e, "bitrate_kbps", path,
                                              o.encoder.bitrate_kbps);
        o.encoder.keyframe_interval_ms =
            rd.integer32(*e, "keyframe_interval_ms", path, o.encoder.keyframe_interval_ms);
        o.encoder.preset = rd.text(*e, "preset", path);
        o.encoder.rate_control = rd.text(*e, "rate_control", path, o.encoder.rate_control);
        o.encoder.extra = dec_params(rd, *e, "extra", path);
    }
    if (const Jv* d = rd.object(v, "destination", path)) {
        o.destination.kind = rd.enumerated(*d, "kind", path, destination_kind_from_string,
                                           o.destination.kind);
        o.destination.url = rd.text(*d, "url", path);
        o.destination.credential_ref = rd.text(*d, "credential_ref", path);
    }
    o.enabled = rd.boolean(v, "enabled", path, false);
    return o;
}

Settings dec_settings(Rd& rd, const Jv& v, std::string_view path) {
    Settings s;
    s.sample_rate = rd.integer32(v, "sample_rate", path, s.sample_rate);
    s.render_queue_frames = rd.integer32(v, "render_queue_frames", path,
                                         s.render_queue_frames);
    s.extra = dec_params(rd, v, "extra", path);
    return s;
}

Source dec_source(Rd& rd, const Jv& v, std::string_view path) {
    Source s;
    s.id = rd.id(v, "id", path);
    s.name = rd.text(v, "name", path);
    s.type = rd.text(v, "type", path);
    s.settings = dec_params(rd, v, "settings", path);
    s.asset = rd.id_opt(v, "asset", path);
    return s;
}

Scene dec_scene(Rd& rd, const Jv& root, std::string_view path) {
    Scene s;
    s.id = rd.id(root, "id", path);
    s.name = rd.text(root, "name", path);
    s.favourite = rd.boolean(root, "favourite", path, false);
    if (const Jv* roots = rd.array(root, "roots", path)) {
        for (const Jv& n : *roots) {
            if (!n.isObject()) {
                rd.fail(path, "a root is not an object");
                break;
            }
            s.roots.push_back(dec_node(rd, n, path));
        }
    }
    if (const Jv* ov = rd.object(root, "audio_overrides", path)) {
        for (const std::string& key : ov->getMemberNames()) {
            const auto node_id = Id::parse(key);
            if (!node_id) {
                rd.fail(path, "an audio override is keyed by something that is not an id");
                break;
            }
            const Jv& e = (*ov)[key];
            if (!e.isObject()) {
                rd.fail(path, "an audio override is not an object");
                break;
            }
            AudioOverride a;
            a.bus = rd.id_opt(e, "bus", path);
            if (!e["gain_db"].isNull())
                a.gain_db = rd.number(e, "gain_db", path, 0.0);
            if (!e["mute"].isNull())
                a.mute = rd.boolean(e, "mute", path, false);
            if (!e["sends"].isNull())
                a.sends = dec_sends(rd, e, "sends", path);
            s.audio_overrides.emplace(*node_id, a);
        }
    }
    return s;
}

/// Deeper than any real scene, shallow enough that the parser's recursion cannot exhaust the
/// stack. A group nested two hundred deep is not a document anyone made on purpose.
constexpr int kMaxNesting = 200;

std::optional<Jv> parse_text(std::string_view text, std::string& error) {
    Json::CharReaderBuilder b;
    b.settings_["stackLimit"] = kMaxNesting;
    const std::unique_ptr<Json::CharReader> reader{b.newCharReader()};
    Jv root;
    std::string errs;
    const char* begin = text.data();
    const char* end = begin + text.size();

    // jsoncpp reports most failures by returning false, but **throws** on a few -- exceeding the
    // nesting limit among them. Letting that escape would turn a corrupt or hostile project file
    // into a crash instead of a refusal, which is the opposite of this format's contract: errors
    // are values, and nothing here throws. Found by the mutation tests, which is what they are
    // for.
    try {
        if (!reader->parse(begin, end, &root, &errs)) {
            error = errs;
            return std::nullopt;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return std::nullopt;
    } catch (...) {
        error = "the file could not be parsed";
        return std::nullopt;
    }
    return root;
}

/// A schema version newer than this build is refused outright rather than partly read. Dropping
/// the fields we do not understand and writing the file back is how a project silently loses
/// work on a downgrade.
bool check_version(Rd& rd, const Jv& root, std::string_view path) {
    const std::int64_t v = rd.integer(root, "schema_version", path, 0);
    if (v <= 0) {
        rd.fail(path, "no schema_version");
        return false;
    }
    if (v > kSchemaVersion) {
        rd.fail(path, std::format("schema_version {} is newer than this build understands ({})",
                                  v, kSchemaVersion));
        return false;
    }
    return true;
}

} // namespace detail

std::expected<SceneFile, std::string> scene_from_json(std::string_view text) {
    std::string parse_error;
    const auto root = parse_text(text, parse_error);
    if (!root)
        return std::unexpected(parse_error);
    if (!root->isObject())
        return std::unexpected("the scene file is not a JSON object");

    Rd rd;
    if (!check_version(rd, *root, "scene"))
        return std::unexpected(rd.error());

    SceneFile out;
    const std::int64_t gen = rd.integer(*root, "generation", "scene", 0);
    out.generation = gen < 0 ? 0 : static_cast<std::uint64_t>(gen);
    out.scene = dec_scene(rd, *root, "scene");
    if (!rd.ok())
        return std::unexpected(rd.error());
    return out;
}

std::expected<std::vector<Asset>, std::string> assets_from_json(std::string_view text) {
    std::string parse_error;
    const auto root = parse_text(text, parse_error);
    if (!root)
        return std::unexpected(parse_error);
    if (!root->isObject())
        return std::unexpected("the asset manifest is not a JSON object");

    Rd rd;
    if (!check_version(rd, *root, "assets"))
        return std::unexpected(rd.error());

    std::vector<Asset> out;
    if (const Jv* a = rd.array(*root, "assets", "assets")) {
        for (const Jv& v : *a) {
            if (!v.isObject()) {
                rd.fail("assets", "an asset is not an object");
                break;
            }
            Asset asset;
            asset.id = rd.id(v, "id", "assets");
            asset.path = rd.text(v, "path", "assets");
            asset.sha256 = rd.text(v, "sha256", "assets");
            const std::int64_t bytes = rd.integer(v, "bytes", "assets", 0);
            asset.bytes = bytes < 0 ? 0 : static_cast<std::uint64_t>(bytes);
            if (const Jv* hints = rd.array(v, "relocation_hints", "assets"))
                for (const Jv& h : *hints)
                    if (h.isString())
                        asset.relocation_hints.push_back(h.asString());
            out.push_back(std::move(asset));
        }
    }
    if (!rd.ok())
        return std::unexpected(rd.error());
    return out;
}

std::expected<ProjectFile, std::string> project_from_json(std::string_view text) {
    std::string parse_error;
    const auto root = parse_text(text, parse_error);
    if (!root)
        return std::unexpected(parse_error);
    if (!root->isObject())
        return std::unexpected("the project file is not a JSON object");

    Rd rd;
    if (!check_version(rd, *root, "project"))
        return std::unexpected(rd.error());

    Project p;
    p.schema_version = static_cast<int>(rd.integer(*root, "schema_version", "project",
                                                   kSchemaVersion));
    p.id = rd.id(*root, "id", "project");
    p.name = rd.text(*root, "name", "project");
    const std::int64_t gen = rd.integer(*root, "generation", "project", 0);
    p.generation = gen < 0 ? 0 : static_cast<std::uint64_t>(gen);

    if (const Jv* a = rd.array(*root, "canvases", "project")) {
        for (const Jv& v : *a)
            p.canvases.push_back(dec_canvas(rd, v, "project.canvases"));
    }

    if (const Jv* a = rd.array(*root, "buses", "project")) {
        for (const Jv& v : *a)
            p.buses.push_back(dec_bus(rd, v, "project.buses"));
    }

    if (const Jv* a = rd.array(*root, "decks", "project")) {
        for (const Jv& v : *a) {
            p.decks.push_back(dec_deck(rd, v, "project.decks"));
        }
    }

    if (const Jv* a = rd.array(*root, "sources", "project")) {
        for (const Jv& v : *a)
            p.sources.push_back(dec_source(rd, v, "project.sources"));
    }

    if (const Jv* a = rd.array(*root, "outputs", "project")) {
        for (const Jv& v : *a)
            p.outputs.push_back(dec_output(rd, v, "project.outputs"));
    }

    if (const Jv* sv = rd.object(*root, "settings", "project"))
        p.settings = dec_settings(rd, *sv, "project.settings");

    std::vector<SceneRef> scene_refs;
    if (const Jv* a = rd.array(*root, "scenes", "project")) {
        for (const Jv& v : *a) {
            if (!v.isObject()) {
                rd.fail("project.scenes", "a scene entry is not an object");
                break;
            }
            SceneRef ref;
            ref.id = rd.id(v, "id", "project.scenes");
            ref.name = rd.text(v, "name", "project.scenes");
            const std::int64_t g = rd.integer(v, "generation", "project.scenes", 0);
            ref.generation = g < 0 ? 0 : static_cast<std::uint64_t>(g);
            scene_refs.push_back(std::move(ref));
        }
    }

    if (!rd.ok())
        return std::unexpected(rd.error());
    return ProjectFile{std::move(p), std::move(scene_refs)};
}

} // namespace ldoc
