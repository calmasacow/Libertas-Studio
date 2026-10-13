// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The command bus: every mutation validated, reversible and serialisable.

#include "ldoc/command.hpp"
#include "ldoc/history.hpp"

#include "ltest.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace ldoc;

namespace {

/// A project with a couple of scenes, a group with children, and an effect to poke at.
struct Fixture {
    Project project;
    Id scene;
    Id group;
    Id child_a;
    Id child_b;
    Id loose;
    Id effect;
    Id canvas;
    Id deck;
    Id existing_source;
    Id spare_canvas;
    Id existing_bus;
    Id existing_output;

    Fixture() {
        project = make_default_project("Show");
        canvas = project.canvases[0].id;
        deck = project.decks[0].id;
        scene = project.scenes[0].id;

        Node g = make_node(NodeKind::Group, "Group");
        group = g.id;
        Node a = make_node(NodeKind::Layer, "Camera");
        child_a = a.id;
        Node b = make_node(NodeKind::Layer, "Overlay");
        child_b = b.id;
        g.children()->push_back(std::move(a));
        g.children()->push_back(std::move(b));

        Node l = make_node(NodeKind::Layer, "Background");
        loose = l.id;
        Effect e;
        e.id = Id::generate();
        e.type = "effect.blur";
        e.params.emplace("radius", 4.0);
        effect = e.id;
        l.effects.push_back(std::move(e));

        project.scenes[0].roots.push_back(std::move(g));
        project.scenes[0].roots.push_back(std::move(l));

        Source src;
        src.id = Id::generate();
        src.name = "Desk Cam";
        src.type = "source.v4l2";
        existing_source = src.id;
        project.sources.push_back(std::move(src));

        // A second scene, so reordering is a real move rather than a clamp to where it already
        // is -- which check_reversible would rightly call "applying it changed nothing".
        Scene spare_scene;
        spare_scene.id = Id::generate();
        spare_scene.name = "Second";
        project.scenes.push_back(std::move(spare_scene));

        existing_bus = project.buses.front().id;

        Output out;
        out.id = Id::generate();
        out.name = "YouTube";
        out.canvas = canvas;
        out.encoder.bitrate_kbps = 9000;
        out.destination.url = "rtmps://example/live";
        out.destination.credential_ref = "youtube-main";
        existing_output = out.id;
        project.outputs.push_back(std::move(out));

        // A canvas nothing renders to, so the removal case has something it is allowed to take.
        Canvas spare;
        spare.id = Id::generate();
        spare.name = "Spare";
        spare_canvas = spare.id;
        project.canvases.push_back(std::move(spare));
    }
};

/// Apply, check it took, apply the inverse, check the project is exactly as it started.
///
/// The command is built from a fixture made *here*, because every Fixture mints fresh ULIDs:
/// a command carrying ids from a different one refers to nothing.
void check_reversible(const char* what,
                      const std::function<Command(const Fixture&)>& make_command) {
    Fixture f;
    const Project before = f.project;
    const Command command = make_command(f);

    auto applied = apply(f.project, command);
    REQUIRE_MSG(applied.has_value(), "{}: {}", what, applied.error());
    CHECK_MSG(!(f.project == before), "{}: applying it changed nothing", what);
    CHECK_MSG(!applied->description.empty(), "{}: has no description", what);

    auto undone = apply(f.project, applied->inverse);
    REQUIRE_MSG(undone.has_value(), "{} (inverse): {}", what, undone.error());
    CHECK_MSG(f.project == before, "{}: undoing it did not restore the project exactly", what);

    // And the inverse of the inverse puts it back, which is what redo relies on.
    auto redone = apply(f.project, undone->inverse);
    REQUIRE_MSG(redone.has_value(), "{} (redo): {}", what, redone.error());
}

std::vector<Command> every_command_shape(const Fixture& f) {
    Scene fresh;
    fresh.id = Id::generate();
    fresh.name = "Added";

    Effect e;
    e.id = Id::generate();
    e.type = "effect.stroke";
    e.params.emplace("width", 2.0);

    CanvasOverride ov;
    ov.visible = false;

    ParamMap params;
    params.emplace("radius", 9.0);

    Transform t;
    t.position = Vec2{10.0, 20.0};
    Layout lay;
    lay.fit = FitMode::Cover;
    Blend bl;
    bl.mode = BlendMode::Multiply;
    bl.opacity = 0.5;

    Source src;
    src.id = Id::generate();
    src.name = "Camera";
    src.type = "source.v4l2";
    src.settings.emplace("device", std::string{"/dev/video0"});

    Source changed = f.project.sources.empty() ? src : f.project.sources.front();
    changed.name = "Renamed source";

    Canvas vertical;
    vertical.id = Id::generate();
    vertical.name = "Vertical";
    vertical.width = 1080;
    vertical.height = 1920;

    Canvas resized = f.project.canvases.front();
    resized.width = 2560;
    resized.height = 1440;

    AudioRouting routed;
    routed.bus = f.project.buses.front().id;
    routed.gain_db = -6.0;
    routed.sends.push_back(Send{f.project.buses.back().id, -3.0, true});

    Bus submix;
    submix.id = Id::generate();
    submix.name = "Game";
    submix.kind = BusKind::Submix;

    Bus louder = f.project.buses.front();
    louder.gain_db = -3.0;

    Output twitch;
    twitch.id = Id::generate();
    twitch.name = "Twitch";
    twitch.canvas = f.canvas;
    twitch.encoder.bitrate_kbps = 6000;
    twitch.destination.url = "rtmp://ingest.example/app";

    Output rebitrated = f.project.outputs.front();
    rebitrated.encoder.bitrate_kbps = 12000;

    Settings tuned = f.project.settings;
    tuned.sample_rate = 44100;

    return {
        SetProjectName{"Renamed"},
        AddScene{fresh, 1},
        RemoveScene{f.scene},
        RenameScene{f.scene, "Intro"},
        MoveScene{f.scene, 1},
        AddNode{f.scene, f.group, 1, make_node(NodeKind::Layer, "New")},
        RemoveNode{f.scene, f.child_a},
        MoveNode{f.scene, f.child_a, std::nullopt, 0},
        RenameNode{f.scene, f.loose, "Renamed"},
        SetNodeVisible{f.scene, f.loose, false},
        SetNodeLocked{f.scene, f.loose, true},
        SetNodeTransform{f.scene, f.loose, t},
        SetNodeLayout{f.scene, f.loose, lay},
        SetNodeBlend{f.scene, f.loose, bl},
        SetCanvasOverride{f.scene, f.loose, f.canvas, ov},
        AddEffect{f.scene, f.loose, 0, e},
        RemoveEffect{f.scene, f.loose, f.effect},
        SetEffectParams{f.scene, f.loose, f.effect, params},
        SetEffectEnabled{f.scene, f.loose, f.effect, false},
        SetDeckProgram{f.deck, std::nullopt},
        SetDeckPreview{f.deck, f.scene},
        SetCanvasEmergencyScene{f.canvas, f.scene},
        SetLayerSource{f.scene, f.loose, f.existing_source},
        AddSource{src, 0},
        RemoveSource{f.existing_source},
        SetSource{changed},
        AddCanvas{vertical, 1},
        RemoveCanvas{f.spare_canvas},
        SetCanvas{resized},
        SetNodeAudio{f.scene, f.loose, routed},
        AddBus{submix, 0},
        RemoveBus{f.existing_bus},
        SetBus{louder},
        AddOutput{twitch, 0},
        RemoveOutput{f.existing_output},
        SetOutput{rebitrated},
        SetProjectSettings{tuned},
    };
}

/// A random command that is plausible against the given project. Nothing here guarantees it
/// applies -- half the value is in the ones that get refused.
Command random_command(std::mt19937& rng, const Project& p) {
    // The sequence can legitimately delete every scene, so there is not always one to pick.
    if (p.scenes.empty() || p.canvases.empty() || p.decks.empty()) {
        Scene fresh;
        fresh.id = Id::generate();
        fresh.name = std::format("Scene {}", rng() % 100);
        return AddScene{fresh, 0};
    }
    const Scene& scene = p.scenes[rng() % p.scenes.size()];
    std::vector<Id> nodes;
    collect_defined_ids(scene.roots, nodes);
    const Id node = nodes.empty() ? Id::generate() : nodes[rng() % nodes.size()];
    const Id canvas = p.canvases[rng() % p.canvases.size()].id;
    const Id deck = p.decks[rng() % p.decks.size()].id;
    // A sequence can remove every bus, which the project is allowed to be left with -- so
    // unlike a canvas or a deck there is not always one to route at.
    const Id bus = p.buses.empty() ? Id::generate() : p.buses[rng() % p.buses.size()].id;

    switch (rng() % 26) {
    case 0:
        return SetProjectName{std::format("Show {}", rng() % 100)};
    case 1: {
        Scene fresh;
        fresh.id = Id::generate();
        fresh.name = std::format("Scene {}", rng() % 100);
        if (rng() % 2)
            fresh.roots.push_back(make_node(NodeKind::Layer, "l"));
        return AddScene{fresh, rng() % (p.scenes.size() + 1)};
    }
    case 2:
        return RemoveScene{scene.id};
    case 3:
        return RenameScene{scene.id, std::format("Scene {}", rng() % 100)};
    case 16:
        return MoveScene{scene.id, rng() % (p.scenes.size() + 2)};
    case 4: {
        const NodeKind kind = (rng() % 4 == 0) ? NodeKind::Group : NodeKind::Layer;
        return AddNode{scene.id, (rng() % 2) ? std::optional<Id>{node} : std::nullopt,
                       rng() % 3, make_node(kind, std::format("n{}", rng() % 100))};
    }
    case 5:
        return RemoveNode{scene.id, node};
    case 6:
        return MoveNode{scene.id, node, (rng() % 2) ? std::optional<Id>{node} : std::nullopt,
                        rng() % 3};
    case 7:
        return RenameNode{scene.id, node, std::format("n{}", rng() % 100)};
    case 8:
        return SetNodeVisible{scene.id, node, (rng() % 2) == 0};
    case 9:
        return SetNodeLocked{scene.id, node, (rng() % 2) == 0};
    case 10: {
        Transform t;
        t.position = Vec2{static_cast<double>(rng() % 1000), static_cast<double>(rng() % 1000)};
        t.rotation_deg = static_cast<double>(rng() % 360);
        return SetNodeTransform{scene.id, node, t};
    }
    case 11: {
        Blend b;
        b.mode = static_cast<BlendMode>(rng() % 25);
        b.opacity = static_cast<double>(rng() % 101) / 100.0;
        return SetNodeBlend{scene.id, node, b};
    }
    case 12: {
        CanvasOverride ov;
        ov.visible = (rng() % 2) == 0;
        return SetCanvasOverride{scene.id, node, canvas,
                                 (rng() % 3) ? std::optional<CanvasOverride>{ov} : std::nullopt};
    }
    case 13: {
        Effect e;
        e.id = Id::generate();
        e.type = "effect.blur";
        e.params.emplace("radius", static_cast<double>(rng() % 40));
        return AddEffect{scene.id, node, rng() % 2, e};
    }
    case 14:
        return SetDeckProgram{deck, (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    case 15:
        return SetCanvasEmergencyScene{canvas,
                                       (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    case 24: {
        AudioRouting a;
        if (rng() % 4)
            a.bus = bus;
        a.gain_db = static_cast<double>(rng() % 24) - 12.0;
        a.mute = (rng() % 2) == 0;
        return SetNodeAudio{scene.id, node, a};
    }
    case 17: {
        Bus b;
        b.id = Id::generate();
        b.name = std::format("Bus {}", rng() % 100);
        b.kind = static_cast<BusKind>(rng() % 3);
        b.gain_db = static_cast<double>(rng() % 24) - 12.0;
        return AddBus{b, rng() % (p.buses.size() + 1)};
    }
    case 18:
        return RemoveBus{bus};
    case 19: {
        if (p.buses.empty())
            return SetProjectName{"no buses"};
        Bus b = p.buses[rng() % p.buses.size()];
        b.gain_db = static_cast<double>(rng() % 24) - 12.0;
        b.mute = (rng() % 2) == 0;
        return SetBus{b};
    }
    case 20: {
        Output o;
        o.id = Id::generate();
        o.name = std::format("Out {}", rng() % 100);
        o.canvas = canvas;
        o.encoder.bitrate_kbps = 1000 + static_cast<int>(rng() % 20000);
        o.destination.url = "rtmp://example/app";
        return AddOutput{o, rng() % (p.outputs.size() + 1)};
    }
    case 21:
        return RemoveOutput{p.outputs.empty() ? Id::generate()
                                              : p.outputs[rng() % p.outputs.size()].id};
    case 22: {
        if (p.outputs.empty())
            return SetProjectName{"no outputs"};
        Output o = p.outputs[rng() % p.outputs.size()];
        o.encoder.bitrate_kbps = 1000 + static_cast<int>(rng() % 20000);
        o.canvas = canvas;
        return SetOutput{o};
    }
    case 23: {
        Settings st = p.settings;
        st.sample_rate = (rng() % 2) ? 48000 : 44100;
        return SetProjectSettings{st};
    }
    default:
        return SetDeckPreview{deck, (rng() % 3) ? std::optional<Id>{scene.id} : std::nullopt};
    }
}

} // namespace

LTEST_CASE(every_command_applies_and_reverses_exactly) {
    const Fixture f;
    const auto commands = every_command_shape(f);
    CHECK_MSG(commands.size() == std::variant_size_v<Command>,
              "{} of {} command types are covered", commands.size(),
              std::variant_size_v<Command>);
    for (std::size_t i = 0; i < commands.size(); ++i) {
        const std::string name{command_name(commands[i])};
        check_reversible(name.c_str(),
                         [i](const Fixture& fx) { return every_command_shape(fx)[i]; });
    }
}

LTEST_CASE(every_command_has_a_distinct_name_and_a_description) {
    const Fixture f;
    std::set<std::string> names;
    for (const Command& c : every_command_shape(f)) {
        const std::string name{command_name(c)};
        CHECK_MSG(names.insert(name).second, "'{}' is used by two commands", name);
        CHECK(!name.empty());
        CHECK_MSG(!describe(c).empty(), "{} has no description", name);
    }
    CHECK_EQ(names.size(), std::variant_size_v<Command>);
}

LTEST_CASE(a_refused_command_changes_nothing) {
    Fixture f;
    const Id nowhere = Id::generate();

    // Each of these is missing the thing it would act on, which is a precondition rather than a
    // reference, so each is refused.
    const std::vector<Command> bad = {
        RenameScene{nowhere, "x"},
        RemoveScene{nowhere},
        AddNode{nowhere, std::nullopt, 0, make_node(NodeKind::Layer, "n")},
        RemoveNode{f.scene, nowhere},
        RenameNode{f.scene, nowhere, "x"},
        SetNodeVisible{f.scene, nowhere, false},
        SetNodeLocked{f.scene, nowhere, true},
        SetEffectParams{f.scene, f.loose, nowhere, {}},
        SetEffectEnabled{f.scene, f.loose, nowhere, false},
        RemoveEffect{f.scene, f.loose, nowhere},
        SetDeckProgram{nowhere, std::nullopt},
        SetDeckPreview{nowhere, std::nullopt},
        SetCanvasOverride{f.scene, nowhere, f.canvas, CanvasOverride{}},
        AddScene{Scene{}, 99},
        AddNode{f.scene, std::nullopt, 99, make_node(NodeKind::Layer, "n")},
        AddEffect{f.scene, nowhere, 0, Effect{}},
    };
    for (const Command& c : bad) {
        const Project before = f.project;
        const auto r = apply(f.project, c);
        CHECK_MSG(!r.has_value(), "{} was accepted when it should not have been",
                  command_name(c));
        CHECK_MSG(f.project == before, "{} mutated the project before failing",
                  command_name(c));
    }
}

LTEST_CASE(pointing_at_something_that_does_not_exist_is_allowed_and_reported_later) {
    // The other side of the precondition/reference line. These produce a document the validator
    // complains about, and that is the right place for the complaint: refusing them here would
    // make undo unable to restore a state the model permits, and would mean nothing could be
    // deleted while anything still named it.
    Fixture f;
    const Id nowhere = Id::generate();

    REQUIRE(apply(f.project, SetDeckProgram{f.deck, nowhere}).has_value());
    CHECK_MSG(is_valid(f.project), "a dangling deck pointer was treated as unrenderable");
    CHECK(!validate(f.project).empty());

    CanvasOverride ov;
    ov.visible = false;
    REQUIRE(apply(f.project, SetCanvasOverride{f.scene, f.loose, nowhere, ov}).has_value());
    CHECK(is_valid(f.project));
    const auto problems = validate(f.project);
    CHECK(std::all_of(problems.begin(), problems.end(), [](const Problem& p) {
        return p.severity == Severity::Warning;
    }));
    CHECK(problems.size() >= 2);
}

LTEST_CASE(a_node_cannot_be_added_with_an_id_the_project_already_uses) {
    // Two nodes with one id is a corruption the validator would catch later; the command should
    // never be able to create it.
    Fixture f;
    const Project before = f.project;
    Node clash = make_node(NodeKind::Layer, "clash");
    clash.id = f.child_a;
    const auto r = apply(f.project, AddNode{f.scene, std::nullopt, 0, clash});
    CHECK(!r.has_value());
    CHECK(r.error().find("already in the project") != std::string::npos);
    CHECK(f.project == before);

    // Including an id buried inside the subtree being added.
    Node g = make_node(NodeKind::Group, "g");
    Node inner = make_node(NodeKind::Layer, "inner");
    inner.id = f.loose;
    g.children()->push_back(std::move(inner));
    CHECK(!apply(f.project, AddNode{f.scene, std::nullopt, 0, g}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(only_a_group_can_be_added_to) {
    Fixture f;
    const Project before = f.project;
    const auto r = apply(f.project, AddNode{f.scene, f.loose, 0,
                                            make_node(NodeKind::Layer, "n")});
    CHECK(!r.has_value());
    CHECK(r.error().find("not a group") != std::string::npos);
    CHECK(f.project == before);
}

LTEST_CASE(a_group_cannot_be_moved_inside_itself) {
    // It would detach the whole subtree and leave the tree referring to nodes nothing owns.
    Fixture f;
    const Project before = f.project;

    const auto into_self = apply(f.project, MoveNode{f.scene, f.group, f.group, 0});
    CHECK(!into_self.has_value());
    CHECK(f.project == before);

    const auto into_child = apply(f.project, MoveNode{f.scene, f.group, f.child_a, 0});
    CHECK_MSG(!into_child.has_value(), "a group was moved inside its own child");
    CHECK(f.project == before);

    // Moving a child out to the root is fine, and reverses.
    check_reversible("move out", [](const Fixture& fx) {
        return Command{MoveNode{fx.scene, fx.child_a, std::nullopt, 0}};
    });
}

LTEST_CASE(removing_a_group_takes_its_children_and_undo_brings_them_all_back) {
    Fixture f;
    const Project before = f.project;
    auto applied = apply(f.project, RemoveNode{f.scene, f.group});
    REQUIRE(applied.has_value());

    std::vector<Id> left;
    collect_defined_ids(f.project.scenes[0].roots, left);
    CHECK(std::find(left.begin(), left.end(), f.child_a) == left.end());
    CHECK(std::find(left.begin(), left.end(), f.child_b) == left.end());

    REQUIRE(apply(f.project, applied->inverse).has_value());
    CHECK_MSG(f.project == before, "undoing a group removal did not restore its children");
}

LTEST_CASE(an_opacity_outside_the_legal_range_is_refused) {
    Fixture f;
    const Project before = f.project;
    Blend b;
    b.opacity = 1.5;
    CHECK(!apply(f.project, SetNodeBlend{f.scene, f.loose, b}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(every_command_survives_a_json_round_trip) {
    const Fixture f;
    for (const Command& c : every_command_shape(f)) {
        const std::string text = command_to_json(c);
        const auto back = command_from_json(text);
        REQUIRE_MSG(back.has_value(), "{}: {}", command_name(c), back.error());
        CHECK_MSG(*back == c, "{} changed on the way through JSON", command_name(c));
        CHECK_MSG(command_to_json(*back) == text, "{} re-serialised differently",
                  command_name(c));
        CHECK(text.find(std::string{command_name(c)}) != std::string::npos);
    }
}

LTEST_CASE(a_command_this_build_does_not_know_is_refused) {
    // Skipping it would make a journal replay a different session than it recorded.
    const auto r = command_from_json(R"({"cmd": "node.teleport", "scene": "x"})");
    CHECK(!r.has_value());
    CHECK(r.error().find("node.teleport") != std::string::npos);

    for (const std::string& bad : {std::string{"{}"}, std::string{"not json"},
                                   std::string{R"({"cmd": 7})"},
                                   std::string{R"({"cmd": "scene.rename"})"},
                                   std::string{R"({"cmd": "scene.rename", "scene": "nope"})"}})
        CHECK_MSG(!command_from_json(bad).has_value(), "accepted {}", bad);
}

LTEST_CASE(history_undoes_and_redoes_in_order_with_descriptions) {
    Fixture f;
    DocumentHistory h{f.project};
    const Project start = h.project();

    REQUIRE(h.run(RenameNode{f.scene, f.loose, "First"}).has_value());
    REQUIRE(h.run(SetNodeVisible{f.scene, f.loose, false}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{2});
    // The description names the layer as it was called *when the command ran*, which is after
    // the rename above.
    CHECK_EQ(h.undo_description(), std::string("Hide 'First'"));

    REQUIRE(h.undo().has_value());
    CHECK(find_node(h.project().scenes[0].roots, f.loose)->visible);
    CHECK_EQ(h.undo_description(), std::string("Rename layer to 'First'"));
    CHECK_EQ(h.redo_description(), std::string("Hide 'First'"));

    REQUIRE(h.undo().has_value());
    CHECK_MSG(h.project() == start, "undoing everything did not restore the project");
    CHECK(!h.can_undo());
    CHECK(!h.undo().has_value());

    REQUIRE(h.redo().has_value());
    REQUIRE(h.redo().has_value());
    CHECK_EQ(find_node(h.project().scenes[0].roots, f.loose)->name, std::string("First"));
    CHECK(!find_node(h.project().scenes[0].roots, f.loose)->visible);
    CHECK(!h.redo().has_value());
}

LTEST_CASE(a_refused_command_does_not_land_on_the_undo_stack) {
    // Otherwise Ctrl+Z after a command that did nothing undoes the previous real one.
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(RenameNode{f.scene, f.loose, "Kept"}).has_value());
    CHECK(!h.run(RenameNode{f.scene, Id::generate(), "x"}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{1});
    CHECK_EQ(h.undo_description(), std::string("Rename layer to 'Kept'"));
}

LTEST_CASE(a_new_command_discards_the_redo_tail) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetNodeVisible{f.scene, f.loose, false}).has_value());
    REQUIRE(h.undo().has_value());
    CHECK(h.can_redo());
    REQUIRE(h.run(RenameNode{f.scene, f.loose, "Branch"}).has_value());
    CHECK(!h.can_redo());
}

LTEST_CASE(the_undo_stack_is_bounded) {
    Fixture f;
    DocumentHistory h{f.project, 8};
    for (int i = 0; i < 40; ++i)
        REQUIRE(h.run(RenameNode{f.scene, f.loose, std::format("n{}", i)}).has_value());
    CHECK_EQ(h.undo_depth(), std::size_t{8});
    for (int i = 0; i < 8; ++i)
        CHECK(h.undo().has_value());
    CHECK(!h.can_undo());
    // Whatever is left is still a coherent project.
    CHECK(is_valid(h.project()));
}

LTEST_CASE(undo_can_restore_a_reference_that_has_since_become_dangling) {
    // Found by the random sequences below, and subtle enough to pin down on its own.
    //
    // Deleting a scene the deck points at is allowed (the deck then shows nothing, which the
    // validator reports as a warning). Pointing the deck somewhere else captures the *old*
    // pointer as its inverse -- and that pointer is now dangling. Undo still has to restore it,
    // because that is the state it is undoing. So a command may only refuse what the document
    // model itself refuses.
    Fixture f;
    DocumentHistory h{f.project};

    Scene spare;
    spare.id = Id::generate();
    spare.name = "Spare";
    const Id spare_id = spare.id;
    REQUIRE(h.run(AddScene{spare, 1}).has_value());
    REQUIRE(h.run(SetDeckProgram{f.deck, spare_id}).has_value());

    // Delete the scene the deck is pointing at, then point the deck elsewhere.
    REQUIRE(h.run(RemoveScene{spare_id}).has_value());
    const Project dangling = h.project();
    CHECK(h.project().decks[0].program_scene.value() == spare_id);
    CHECK_MSG(is_valid(h.project()), "a dangling deck pointer was treated as an error");

    REQUIRE(h.run(SetDeckProgram{f.deck, f.scene}).has_value());
    const auto undone = h.undo();
    REQUIRE_MSG(undone.has_value(), "{}", undone ? std::string{} : undone.error());
    CHECK_MSG(h.project() == dangling, "undo did not restore the pointer it replaced");
}

LTEST_CASE(random_command_sequences_undo_and_redo_to_the_exact_same_project) {
    // The property the plan asks for by name. Random sequences, because the interesting failures
    // are in the orderings nobody writes a case for -- a remove whose undo has to restore a
    // subtree into the right parent at the right index, after other things have moved.
    std::mt19937 rng{20261012u};

    for (int iteration = 0; iteration < 120; ++iteration) {
        Fixture f;
        DocumentHistory h{f.project, 10000};
        const Project start = h.project();

        int applied = 0;
        int refused = 0;
        for (int step = 0; step < 60; ++step) {
            const Command c = random_command(rng, h.project());
            const Project before = h.project();
            if (h.run(c)) {
                ++applied;
            } else {
                ++refused;
                REQUIRE_MSG(h.project() == before,
                            "iteration {} step {}: a refused {} mutated the project", iteration,
                            step, command_name(c));
            }
            const auto bad = validate(h.project());
            const bool has_error = std::any_of(bad.begin(), bad.end(), [](const Problem& x) {
                return x.severity == Severity::Error;
            });
            REQUIRE_MSG(!has_error, "iteration {} step {}: {} left the project invalid: {}",
                        iteration, step, command_name(c),
                        bad.empty() ? std::string{} : bad.front().message);
        }
        REQUIRE_MSG(applied > 10, "iteration {}: only {} commands applied", iteration, applied);

        const Project finished = h.project();

        // Undo everything: the project must be byte-identical to where it started.
        const std::size_t depth = h.undo_depth();
        for (std::size_t i = 0; i < depth; ++i) {
            const std::string what = h.undo_description();
            const auto r = h.undo();
            REQUIRE_MSG(r.has_value(), "iteration {}: undo {} ('{}') failed: {}", iteration, i,
                        what, r ? std::string{} : r.error());
        }
        REQUIRE_MSG(h.project() == start,
                    "iteration {}: undoing {} command(s) did not restore the project",
                    iteration, depth);

        // Redo everything: and identical to where it ended.
        for (std::size_t i = 0; i < depth; ++i)
            REQUIRE_MSG(h.redo().has_value(), "iteration {}: redo {} failed", iteration, i);
        REQUIRE_MSG(h.project() == finished,
                    "iteration {}: redoing {} command(s) did not restore the project",
                    iteration, depth);
        (void)refused;
    }
}

LTEST_CASE(every_applied_command_survives_the_journal_shaped_round_trip) {
    // Each command is applied, serialised, parsed back and applied to a fresh copy; the two
    // projects must agree. This is replay, which is what the journal does after a crash.
    std::mt19937 rng{99u};
    Fixture f;
    Project live = f.project;
    Project replayed = f.project;

    int applied = 0;
    for (int step = 0; step < 400; ++step) {
        const Command c = random_command(rng, live);
        const auto direct = apply(live, c);
        if (!direct)
            continue;
        ++applied;

        const auto text = command_to_json(c);
        const auto parsed = command_from_json(text);
        REQUIRE_MSG(parsed.has_value(), "step {}: {}", step, parsed.error());
        const auto second = apply(replayed, *parsed);
        REQUIRE_MSG(second.has_value(), "step {}: replaying {} failed: {}", step,
                    command_name(c), second.error());
        REQUIRE_MSG(live == replayed, "step {}: replaying {} diverged", step, command_name(c));
    }
    CHECK(applied > 100);
}

LTEST_CASE(designating_an_emergency_scene_is_an_ordinary_undoable_command) {
    Fixture f;
    DocumentHistory h{f.project};
    CHECK(!h.project().canvases[0].emergency_scene.has_value());

    REQUIRE(h.run(SetCanvasEmergencyScene{f.canvas, f.scene}).has_value());
    CHECK(h.project().canvases[0].emergency_scene.value() == f.scene);
    CHECK_EQ(h.undo_description(), std::string("Set emergency scene"));

    REQUIRE(h.undo().has_value());
    CHECK_MSG(!h.project().canvases[0].emergency_scene.has_value(),
              "undoing the designation did not clear it");
    REQUIRE(h.redo().has_value());
    CHECK(h.project().canvases[0].emergency_scene.value() == f.scene);

    // Clearing it is the same command with nothing in it.
    REQUIRE(h.run(SetCanvasEmergencyScene{f.canvas, std::nullopt}).has_value());
    CHECK(!h.project().canvases[0].emergency_scene.has_value());
    CHECK_EQ(h.undo_description(), std::string("Clear emergency scene"));

    // The canvas is a precondition; the scene is not. ADR-0023: refusing a state the model
    // permits would make an inverse unapplicable and break undo at an arbitrary depth. Judging
    // whether a scene is a *suitable* safety net belongs to the designation flow, which runs
    // check_emergency_scene before it ever issues this.
    CHECK(!apply(f.project, SetCanvasEmergencyScene{Id::generate(), f.scene}).has_value());
    CHECK(apply(f.project, SetCanvasEmergencyScene{f.canvas, Id::generate()}).has_value());
}

LTEST_CASE(a_layer_can_be_bound_to_a_source_and_unbound) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetLayerSource{f.scene, f.loose, f.existing_source}).has_value());
    const Node* n = find_node(h.project().scenes[0].roots, f.loose);
    REQUIRE(n != nullptr);
    CHECK(std::get<LayerData>(n->data).source.value() == f.existing_source);

    REQUIRE(h.undo().has_value());
    CHECK(!std::get<LayerData>(find_node(h.project().scenes[0].roots, f.loose)->data)
               .source.has_value());

    // A group is not a layer, so there is nothing to bind.
    CHECK(!apply(f.project, SetLayerSource{f.scene, f.group, f.existing_source}).has_value());
}

LTEST_CASE(a_canvas_that_something_renders_to_cannot_be_removed) {
    // Unlike a source, a deck or output without a canvas has no output surface at all -- which
    // validate() calls an error, so refusing here is the model's own rule rather than an extra
    // one. ADR-0023's line again: a command refuses exactly what the model refuses.
    Fixture f;
    const Project before = f.project;
    const auto r = apply(f.project, RemoveCanvas{f.canvas});
    CHECK(!r.has_value());
    CHECK(r.error().find("point it at another one first") != std::string::npos);
    CHECK(f.project == before);

    // The spare one, which nothing renders to, comes away cleanly.
    check_reversible("remove spare canvas", [](const Fixture& fx) {
        return Command{RemoveCanvas{fx.spare_canvas}};
    });
}

LTEST_CASE(removing_a_source_leaves_its_layers_pointing_at_nothing_and_says_so) {
    Fixture f;
    DocumentHistory h{f.project};
    REQUIRE(h.run(SetLayerSource{f.scene, f.loose, f.existing_source}).has_value());
    REQUIRE(h.run(RemoveSource{f.existing_source}).has_value());

    CHECK_MSG(is_valid(h.project()), "a dangling source reference was treated as unrenderable");
    const auto problems = validate(h.project());
    CHECK(!problems.empty());
    CHECK(problems.front().severity == Severity::Warning);

    // And undoing brings the source back with the binding intact.
    REQUIRE(h.undo().has_value());
    CHECK(h.project().source(f.existing_source) != nullptr);
    CHECK(validate(h.project()).empty());
}

LTEST_CASE(a_canvas_cannot_be_given_an_impossible_size_or_rate) {
    Fixture f;
    const Project before = f.project;
    Canvas bad = f.project.canvases.front();
    bad.width = 0;
    CHECK(!apply(f.project, SetCanvas{bad}).has_value());

    bad = f.project.canvases.front();
    bad.fps = Fps{30, 0};
    CHECK(!apply(f.project, SetCanvas{bad}).has_value());
    CHECK(f.project == before);
}

LTEST_CASE(an_output_needs_a_canvas_but_a_bus_does_not_need_its_listeners) {
    // The same line again, landing differently because the model does. An output with no canvas
    // has nothing to encode, which validate() calls an error, so the command refuses it. A bus
    // that something still routes to is only a dangling reference -- a warning -- so removing it
    // is allowed and the complaint comes later.
    Fixture f;
    const Project before = f.project;

    Output orphan;
    orphan.id = Id::generate();
    orphan.name = "Nowhere";
    orphan.canvas = Id::generate();
    orphan.encoder.bitrate_kbps = 6000;
    CHECK(!apply(f.project, AddOutput{orphan, 0}).has_value());
    CHECK(f.project == before);

    Output broke = f.project.outputs.front();
    broke.encoder.bitrate_kbps = 0;
    CHECK(!apply(f.project, SetOutput{broke}).has_value());

    Output live = f.project.outputs.front();
    live.enabled = true;
    live.destination.url.clear();
    CHECK_MSG(!apply(f.project, SetOutput{live}).has_value(),
              "an output was enabled with nowhere to send to");
    CHECK(f.project == before);

    // Removing a bus a layer routes to is allowed, and reported afterwards.
    DocumentHistory h{f.project};
    Node mic = make_node(NodeKind::Layer, "Mic");
    std::get<LayerData>(mic.data).audio.bus = f.existing_bus;
    REQUIRE(h.run(AddNode{f.scene, std::nullopt, 0, std::move(mic)}).has_value());
    REQUIRE(h.run(RemoveBus{f.existing_bus}).has_value());
    CHECK_MSG(is_valid(h.project()), "a dangling bus reference was treated as unrenderable");
    CHECK(!validate(h.project()).empty());
}

LTEST_CASE(project_settings_are_undoable_and_bounded) {
    Fixture f;
    DocumentHistory h{f.project};
    Settings bad = f.project.settings;
    bad.sample_rate = 0;
    CHECK(!h.run(SetProjectSettings{bad}).has_value());

    Settings good = f.project.settings;
    good.sample_rate = 44100;
    REQUIRE(h.run(SetProjectSettings{good}).has_value());
    CHECK_EQ(h.project().settings.sample_rate, 44100);
    REQUIRE(h.undo().has_value());
    CHECK_EQ(h.project().settings.sample_rate, 48000);
}

LTEST_MAIN("command")
