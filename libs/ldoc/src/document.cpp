// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/document.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ldoc {
namespace {

template <typename T, std::size_t N>
std::optional<T> lookup(const std::array<std::pair<T, std::string_view>, N>& table,
                        std::string_view s) {
    for (const auto& [value, name] : table)
        if (name == s)
            return value;
    return std::nullopt;
}

template <typename T, std::size_t N>
std::string_view name_of(const std::array<std::pair<T, std::string_view>, N>& table, T v) {
    for (const auto& [value, name] : table)
        if (value == v)
            return name;
    return "unknown";
}

constexpr std::array<std::pair<BusKind, std::string_view>, 6> kBusKinds = {{
    {BusKind::Input, "input"},     {BusKind::Submix, "submix"},
    {BusKind::Monitor, "monitor"}, {BusKind::Track, "track"},
    {BusKind::Program, "program"}, {BusKind::Vod, "vod"},
}};

constexpr std::array<std::pair<DestinationKind, std::string_view>, 5> kDestinationKinds = {{
    {DestinationKind::Rtmp, "rtmp"}, {DestinationKind::Rtmps, "rtmps"},
    {DestinationKind::Srt, "srt"},   {DestinationKind::Whip, "whip"},
    {DestinationKind::File, "file"},
}};

std::string_view ref_kind_name(RefKind k) {
    switch (k) {
    case RefKind::Node:   return "node";
    case RefKind::Source: return "source";
    case RefKind::Scene:  return "scene";
    case RefKind::Canvas: return "canvas";
    case RefKind::Bus:    return "bus";
    }
    return "thing";
}

/// What owns an id, for a message that names something the operator can find.
struct Owner {
    std::string_view kind;
    std::string name;
};

} // namespace

std::string_view to_string(BusKind k) { return name_of(kBusKinds, k); }
std::optional<BusKind> bus_kind_from_string(std::string_view s) { return lookup(kBusKinds, s); }

std::string_view to_string(DestinationKind k) { return name_of(kDestinationKinds, k); }
std::optional<DestinationKind> destination_kind_from_string(std::string_view s) {
    return lookup(kDestinationKinds, s);
}

const Canvas* Project::canvas(const Id& wanted) const {
    const auto it = std::find_if(canvases.begin(), canvases.end(),
                                 [&wanted](const Canvas& c) { return c.id == wanted; });
    return it == canvases.end() ? nullptr : &*it;
}

const Scene* Project::scene(const Id& wanted) const {
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&wanted](const Scene& s) { return s.id == wanted; });
    return it == scenes.end() ? nullptr : &*it;
}

Scene* Project::scene(const Id& wanted) {
    const auto it =
        std::find_if(scenes.begin(), scenes.end(), [&wanted](const Scene& s) { return s.id == wanted; });
    return it == scenes.end() ? nullptr : &*it;
}

const Bus* Project::bus(const Id& wanted) const {
    const auto it =
        std::find_if(buses.begin(), buses.end(), [&wanted](const Bus& b) { return b.id == wanted; });
    return it == buses.end() ? nullptr : &*it;
}

const Source* Project::source(const Id& wanted) const {
    const auto it = std::find_if(sources.begin(), sources.end(),
                                 [&wanted](const Source& s) { return s.id == wanted; });
    return it == sources.end() ? nullptr : &*it;
}

Project make_default_project(std::string name) {
    Project p;
    p.id = Id::generate();
    p.name = std::move(name);

    Canvas canvas;
    canvas.id = Id::generate();
    canvas.name = "Main";
    canvas.width = 1920;
    canvas.height = 1080;
    canvas.fps = Fps{60, 1};
    p.canvases.push_back(canvas);

    Bus program;
    program.id = Id::generate();
    program.name = "Program";
    program.kind = BusKind::Program;
    p.buses.push_back(program);

    // The copyright-safe recording track, present from the start because adding it later means
    // re-routing every source that already exists.
    Bus vod;
    vod.id = Id::generate();
    vod.name = "VOD";
    vod.kind = BusKind::Vod;
    p.buses.push_back(vod);

    Scene scene;
    scene.id = Id::generate();
    scene.name = "Scene";
    p.scenes.push_back(scene);

    Deck deck;
    deck.id = Id::generate();
    deck.name = "Main";
    deck.canvas = canvas.id;
    deck.program_scene = scene.id;
    p.decks.push_back(deck);

    return p;
}

Scene duplicate_scene(const Scene& scene, std::string new_name) {
    Scene copy;
    copy.id = Id::generate();
    copy.name = std::move(new_name);
    std::map<Id, Id> remap;
    copy.roots = duplicate(scene.roots, &remap);
    // Audio overrides are keyed by layer id, so they have to follow the remap or they would
    // silently apply to nothing.
    for (const auto& [node_id, ov] : scene.audio_overrides) {
        const auto it = remap.find(node_id);
        copy.audio_overrides.emplace(it == remap.end() ? node_id : it->second, ov);
    }
    return copy;
}

// ---- validation -----------------------------------------------------------------------------

namespace {

class Validator {
public:
    explicit Validator(const Project& p) : p_(p) {}

    std::vector<Problem> run() {
        index_ids();
        check_schema();
        check_canvases();
        check_buses();
        check_sources_and_assets();
        check_scenes();
        check_decks();
        check_outputs();
        check_reference_cycles();
        check_precomp_cycles();
        return std::move(problems_);
    }

private:
    void err(const Id& subject, std::string msg) {
        problems_.push_back(Problem{Severity::Error, std::move(msg), subject});
    }
    void warn(const Id& subject, std::string msg) {
        problems_.push_back(Problem{Severity::Warning, std::move(msg), subject});
    }

    void define(const Id& id, std::string_view kind, std::string name) {
        if (id.is_nil()) {
            err(id, std::format("a {} named '{}' has no id", kind, name));
            return;
        }
        const auto [it, fresh] = owners_.emplace(id, Owner{kind, std::move(name)});
        if (!fresh) {
            err(id, std::format("id {} is used by both the {} '{}' and a {}", id.to_string(),
                                it->second.kind, it->second.name, kind));
        }
    }

    void index_ids() {
        define(p_.id, "project", p_.name);
        for (const Canvas& c : p_.canvases)
            define(c.id, "canvas", c.name);
        for (const Bus& b : p_.buses) {
            define(b.id, "bus", b.name);
            for (const Effect& e : b.inserts)
                define(e.id, "insert", e.type);
        }
        for (const Source& s : p_.sources)
            define(s.id, "source", s.name);
        for (const Asset& a : p_.assets)
            define(a.id, "asset", a.path);
        for (const Deck& d : p_.decks)
            define(d.id, "deck", d.name);
        for (const Output& o : p_.outputs)
            define(o.id, "output", o.name);
        for (const Scene& s : p_.scenes) {
            define(s.id, "scene", s.name);
            visit(s.roots, [&](const Node& n) {
                define(n.id, to_string(n.kind()), n.name);
                for (const Effect& e : n.effects)
                    define(e.id, "effect", e.type);
            });
        }
    }

    [[nodiscard]] bool exists_as(const Id& id, std::string_view kind) const {
        const auto it = owners_.find(id);
        return it != owners_.end() && it->second.kind == kind;
    }

    void check_schema() {
        if (p_.schema_version <= 0)
            err(p_.id, std::format("schema_version is {}", p_.schema_version));
        if (p_.schema_version > kSchemaVersion)
            err(p_.id, std::format("schema_version {} is newer than this build understands ({})",
                                   p_.schema_version, kSchemaVersion));
        if (p_.settings.sample_rate <= 0)
            err(p_.id, std::format("sample rate is {}", p_.settings.sample_rate));
        if (p_.canvases.empty())
            warn(p_.id, "the project has no canvases, so nothing can be rendered");
    }

    void check_canvases() {
        for (const Canvas& c : p_.canvases) {
            if (c.width <= 0 || c.height <= 0)
                err(c.id, std::format("canvas '{}' is {}x{}", c.name, c.width, c.height));
            if (!c.fps.valid())
                err(c.id, std::format("canvas '{}' has frame rate {}/{}", c.name, c.fps.num,
                                      c.fps.den));
        }
    }

    void check_buses() {
        for (const Bus& b : p_.buses) {
            if (b.channels <= 0)
                err(b.id, std::format("bus '{}' has {} channels", b.name, b.channels));
            for (const Send& s : b.sends) {
                if (s.to == b.id)
                    err(b.id, std::format("bus '{}' sends to itself", b.name));
                else if (!exists_as(s.to, "bus"))
                    err(b.id, std::format("bus '{}' sends to {}, which is not a bus", b.name,
                                          s.to.to_string()));
            }
            for (const Effect& e : b.inserts) {
                if (e.type.empty())
                    err(e.id, std::format("an insert on bus '{}' has no type", b.name));
            }
        }
    }

    void check_sources_and_assets() {
        for (const Source& s : p_.sources) {
            if (s.type.empty())
                err(s.id, std::format("source '{}' has no type", s.name));
            if (s.asset && !exists_as(*s.asset, "asset"))
                warn(s.id, std::format("source '{}' refers to asset {}, which is not in the "
                                       "manifest",
                                       s.name, s.asset->to_string()));
        }
        for (const Asset& a : p_.assets) {
            if (a.path.empty())
                err(a.id, "an asset has no path");
        }
    }

    void check_scenes() {
        for (const Scene& s : p_.scenes) {
            std::unordered_set<Id> in_scene;
            visit(s.roots, [&](const Node& n) {
                in_scene.insert(n.id);
                if (n.blend.opacity < 0.0 || n.blend.opacity > 1.0)
                    err(n.id, std::format("'{}' has opacity {}", n.name, n.blend.opacity));
                for (const Effect& e : n.effects) {
                    if (e.type.empty())
                        err(e.id, std::format("an effect on '{}' has no type", n.name));
                }
                if (const auto* g = std::get_if<GroupData>(&n.data)) {
                    (void)g;
                } else if (const auto* ref = std::get_if<ReferenceData>(&n.data)) {
                    if (ref->target == n.id)
                        err(n.id, std::format("'{}' references itself", n.name));
                }
            });

            std::vector<Reference> refs;
            collect_references(s.roots, refs);
            for (const Reference& r : refs) {
                const char* kind = nullptr;
                switch (r.kind) {
                case RefKind::Source: kind = "source"; break;
                case RefKind::Scene:  kind = "scene"; break;
                case RefKind::Canvas: kind = "canvas"; break;
                case RefKind::Bus:    kind = "bus"; break;
                case RefKind::Node:   kind = nullptr; break;
                }
                // A dangling reference is a *warning*, deliberately.
                //
                // It is not structurally broken: the node draws nothing, exactly as it would
                // with no reference set at all, and the only difference is that this one records
                // that something was deleted. Making it an error would mean a scene could not be
                // deleted while anything cloned it and a source could not be removed while any
                // layer used it -- a web of refusals that makes ordinary editing a puzzle. The
                // operator is told what is missing and can fix it; nothing silently renders
                // wrong. Errors are reserved for documents that cannot be rendered as written.
                if (kind) {
                    if (!exists_as(r.id, kind))
                        warn(r.from, std::format("refers to {} {}, which does not exist",
                                                 ref_kind_name(r.kind), r.id.to_string()));
                } else {
                    // Node references resolve within the project, not only within this scene: a
                    // Reference may legitimately point at a node in another scene.
                    if (owners_.find(r.id) == owners_.end())
                        warn(r.from,
                             std::format("refers to node {}, which does not exist",
                                         r.id.to_string()));
                }
            }

            for (const auto& [node_id, ov] : s.audio_overrides) {
                if (!in_scene.contains(node_id))
                    warn(s.id, std::format("scene '{}' has audio routing for node {}, which is "
                                           "not in it",
                                           s.name, node_id.to_string()));
                if (ov.bus && !exists_as(*ov.bus, "bus"))
                    warn(s.id, std::format("scene '{}' routes a layer to bus {}, which does not "
                                           "exist",
                                           s.name, ov.bus->to_string()));
            }
        }
    }

    void check_decks() {
        for (const Deck& d : p_.decks) {
            if (!exists_as(d.canvas, "canvas"))
                err(d.id, std::format("deck '{}' targets canvas {}, which does not exist", d.name,
                                      d.canvas.to_string()));
            // Same reasoning as a dangling reference: a deck pointing at a deleted scene shows
            // nothing, which is what a deck with no program scene does anyway. The canvas above
            // is different -- a deck without one has no output surface at all.
            if (d.program_scene && !exists_as(*d.program_scene, "scene"))
                warn(d.id, std::format("deck '{}' has a program scene that was removed", d.name));
            if (d.preview_scene && !exists_as(*d.preview_scene, "scene"))
                warn(d.id, std::format("deck '{}' has a preview scene that was removed", d.name));
            if (d.transition_ms < 0)
                err(d.id, std::format("deck '{}' has a transition of {} ms", d.name,
                                      d.transition_ms));
        }
    }

    void check_outputs() {
        for (const Output& o : p_.outputs) {
            if (!exists_as(o.canvas, "canvas"))
                err(o.id, std::format("output '{}' encodes canvas {}, which does not exist",
                                      o.name, o.canvas.to_string()));
            if (o.encoder.bitrate_kbps <= 0)
                err(o.id, std::format("output '{}' has a bitrate of {} kbps", o.name,
                                      o.encoder.bitrate_kbps));
            if (o.enabled && o.destination.url.empty())
                err(o.id, std::format("output '{}' is enabled but has no destination", o.name));
        }
    }

    /// A Reference pointing, directly or through other References, back at itself would make the
    /// renderer recurse until it died. Catching it here means the renderer never has to carry a
    /// depth limit whose only purpose is to survive a document it should have refused.
    void check_reference_cycles() {
        std::unordered_map<Id, Id> edges;
        for (const Scene& s : p_.scenes) {
            visit(s.roots, [&](const Node& n) {
                if (const auto* ref = std::get_if<ReferenceData>(&n.data))
                    edges.emplace(n.id, ref->target);
            });
        }
        for (const auto& [from, to] : edges) {
            (void)to;
            std::unordered_set<Id> seen{from};
            Id at = from;
            for (;;) {
                const auto it = edges.find(at);
                if (it == edges.end())
                    break;
                at = it->second;
                if (!seen.insert(at).second) {
                    err(from, std::format("reference {} is part of a loop", from.to_string()));
                    break;
                }
            }
        }
    }

    /// The same problem one level up: scene A precomps scene B precomps scene A.
    void check_precomp_cycles() {
        std::unordered_map<Id, std::vector<Id>> edges;
        for (const Scene& s : p_.scenes) {
            visit(s.roots, [&](const Node& n) {
                if (const auto* pre = std::get_if<PrecompData>(&n.data))
                    edges[s.id].push_back(pre->scene);
            });
        }

        std::unordered_set<Id> done;
        std::unordered_set<Id> on_stack;
        const std::function<bool(const Id&)> walk = [&](const Id& scene_id) {
            if (on_stack.contains(scene_id))
                return true;
            if (done.contains(scene_id))
                return false;
            on_stack.insert(scene_id);
            bool cycle = false;
            const auto it = edges.find(scene_id);
            if (it != edges.end()) {
                for (const Id& next : it->second)
                    cycle = walk(next) || cycle;
            }
            on_stack.erase(scene_id);
            done.insert(scene_id);
            return cycle;
        };
        for (const Scene& s : p_.scenes) {
            if (walk(s.id))
                err(s.id, std::format("scene '{}' precomposes itself, directly or through "
                                      "another scene",
                                      s.name));
        }
    }

    const Project& p_;
    std::unordered_map<Id, Owner> owners_;
    std::vector<Problem> problems_;
};

} // namespace

std::vector<Problem> validate(const Project& project) {
    return Validator{project}.run();
}

bool is_valid(const Project& project) {
    const auto problems = validate(project);
    return std::none_of(problems.begin(), problems.end(),
                        [](const Problem& p) { return p.severity == Severity::Error; });
}

} // namespace ldoc
