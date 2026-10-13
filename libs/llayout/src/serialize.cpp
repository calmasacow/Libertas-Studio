// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "llayout/serialize.hpp"

#include "lcore/atomic_file.hpp"

#include <format>
#include <limits>
#include <unordered_map>

#include <toml++/toml.hpp>

namespace llayout {
namespace {

std::string quote(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::expected<NodeId, std::string> to_node_id(std::int64_t v, std::string_view what) {
    if (v <= 0 || v > static_cast<std::int64_t>(std::numeric_limits<NodeId>::max()))
        return std::unexpected(std::format("{} is out of range: {}", what, v));
    return static_cast<NodeId>(v);
}

} // namespace

namespace {

/// The tree itself, with `[[node]]` written under `table_prefix` so that the same emitter serves
/// both a bare layout file and one workspace inside a set.
std::string layout_body(const Layout& l, std::string_view table_prefix) {
    std::string out;
    out += std::format("root = {}\n", l.root());

    for (const NodeId id : l.node_ids()) {
        const Node* n = l.find(id);
        if (!n)
            continue;
        out += std::format("\n[[{}node]]\n", table_prefix);
        out += std::format("id = {}\n", id);
        if (n->is_split()) {
            out += "kind = \"split\"\n";
            out += std::format("axis = \"{}\"\n", to_string(n->axis));
            out += "children = [";
            for (std::size_t i = 0; i < n->children.size(); ++i)
                out += std::format("{}{}", i ? ", " : " ", n->children[i]);
            out += " ]\n";
            out += "fractions = [";
            for (std::size_t i = 0; i < n->fractions.size(); ++i)
                out += std::format("{}{:.12f}", i ? ", " : " ", n->fractions[i]);
            out += " ]\n";
        } else {
            out += "kind = \"tabs\"\n";
            out += "panels = [";
            for (std::size_t i = 0; i < n->tabs.size(); ++i)
                out += std::format("{}{}", i ? ", " : " ", quote(n->tabs[i]));
            out += " ]\n";
            out += std::format("active = {}\n", n->active);
        }
    }
    return out;
}

} // namespace

std::string to_toml(const Layout& l) {
    std::string out;
    out += "# Libertas-Studio workspace layout.\n";
    out += "#\n";
    out += "# Proportions only. No pixel geometry is stored here (ADR-0015 rule 4), so this file\n";
    out += "# restores correctly at any resolution and on any monitor arrangement. It is safe to\n";
    out += "# edit by hand: anything that fails to parse or fails validation is replaced by the\n";
    out += "# built-in default layout, and the application says so rather than restoring\n";
    out += "# something incoherent.\n\n";
    out += std::format("version = {}\n", kLayoutFormatVersion);
    out += layout_body(l, "");
    return out;
}

namespace {
std::expected<Layout, std::string> layout_from_table(const toml::table& tbl);
} // namespace

std::expected<Layout, std::string> from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("line {}: {}", e.source().begin.line,
                                           std::string_view{e.description()}));
    } catch (const std::exception& e) {
        // Same contract as the JSON side: a damaged file is refused, never thrown out of.
        return std::unexpected(e.what());
    } catch (...) {
        return std::unexpected("the file could not be parsed");
    }

    const auto version = tbl["version"].value<std::int64_t>();
    if (!version)
        return std::unexpected("missing 'version'");
    if (*version != kLayoutFormatVersion)
        return std::unexpected(std::format("unsupported layout format version {} (this build "
                                           "reads version {})",
                                           *version, kLayoutFormatVersion));

    return layout_from_table(tbl);
}

namespace {

std::expected<Layout, std::string> layout_from_table(const toml::table& tbl) {
    const auto root_raw = tbl["root"].value<std::int64_t>();
    if (!root_raw)
        return std::unexpected("missing 'root'");
    const auto root = to_node_id(*root_raw, "root");
    if (!root)
        return std::unexpected(root.error());

    const toml::array* arr = tbl["node"].as_array();
    if (!arr || arr->empty())
        return std::unexpected("no [[node]] entries");

    std::unordered_map<NodeId, Node> nodes;
    for (std::size_t i = 0; i < arr->size(); ++i) {
        const toml::table* t = arr->get(i)->as_table();
        if (!t)
            return std::unexpected(std::format("node #{} is not a table", i));

        const auto id_raw = (*t)["id"].value<std::int64_t>();
        if (!id_raw)
            return std::unexpected(std::format("node #{} has no 'id'", i));
        const auto id = to_node_id(*id_raw, std::format("node #{} id", i));
        if (!id)
            return std::unexpected(id.error());

        const auto kind = (*t)["kind"].value<std::string>();
        if (!kind)
            return std::unexpected(std::format("node {} has no 'kind'", *id));

        Node n;
        n.id = *id;

        if (*kind == "split") {
            n.kind = NodeKind::Split;
            const auto axis = (*t)["axis"].value<std::string>();
            if (!axis)
                return std::unexpected(std::format("split {} has no 'axis'", *id));
            if (*axis == "h")
                n.axis = Axis::Horizontal;
            else if (*axis == "v")
                n.axis = Axis::Vertical;
            else
                return std::unexpected(
                    std::format("split {} has axis '{}'; expected \"h\" or \"v\"", *id, *axis));

            const toml::array* kids = (*t)["children"].as_array();
            const toml::array* frs = (*t)["fractions"].as_array();
            if (!kids)
                return std::unexpected(std::format("split {} has no 'children'", *id));
            if (!frs)
                return std::unexpected(std::format("split {} has no 'fractions'", *id));
            if (kids->size() != frs->size())
                return std::unexpected(std::format(
                    "split {} has {} children but {} fractions", *id, kids->size(), frs->size()));

            for (std::size_t j = 0; j < kids->size(); ++j) {
                const auto c_raw = kids->get(j)->value<std::int64_t>();
                if (!c_raw)
                    return std::unexpected(
                        std::format("split {} child #{} is not an integer", *id, j));
                const auto c = to_node_id(*c_raw, std::format("split {} child #{}", *id, j));
                if (!c)
                    return std::unexpected(c.error());
                n.children.push_back(*c);

                const auto f = frs->get(j)->value<double>();
                if (!f)
                    return std::unexpected(
                        std::format("split {} fraction #{} is not a number", *id, j));
                n.fractions.push_back(*f);
            }
        } else if (*kind == "tabs") {
            n.kind = NodeKind::Tabs;
            const toml::array* panels = (*t)["panels"].as_array();
            if (!panels)
                return std::unexpected(std::format("tab group {} has no 'panels'", *id));
            for (std::size_t j = 0; j < panels->size(); ++j) {
                const auto p = panels->get(j)->value<std::string>();
                if (!p)
                    return std::unexpected(
                        std::format("tab group {} panel #{} is not a string", *id, j));
                if (p->empty())
                    return std::unexpected(
                        std::format("tab group {} panel #{} is empty", *id, j));
                n.tabs.push_back(*p);
            }
            const auto active = (*t)["active"].value<std::int64_t>();
            n.active = (active && *active >= 0) ? static_cast<std::size_t>(*active) : 0;
        } else {
            return std::unexpected(
                std::format("node {} has kind '{}'; expected \"split\" or \"tabs\"", *id, *kind));
        }

        if (!nodes.emplace(*id, std::move(n)).second)
            return std::unexpected(std::format("node id {} appears more than once", *id));
    }

    Layout l;
    if (!l.adopt(std::move(nodes), *root))
        return std::unexpected("the nodes do not form a single valid tree rooted at 'root'");
    return l;
}

} // namespace

std::string to_toml(const Workspaces& w) {
    std::string out;
    out += "# Libertas-Studio saved workspaces.\n";
    out += "#\n";
    out += "# Each [[workspace]] is one saved arrangement -- which panels are on screen and how\n";
    out += "# they are divided. These are not compositor workspaces: nothing here is an OS\n";
    out += "# window (ADR-0015).\n";
    out += "#\n";
    out += "# Proportions only. No pixel geometry is stored (ADR-0015 rule 4), so this restores\n";
    out += "# correctly at any resolution and on any monitor arrangement. Safe to edit by hand:\n";
    out += "# anything that fails to parse or validate is replaced by the defaults, and the\n";
    out += "# application says so rather than restoring something incoherent.\n\n";
    out += std::format("version = {}\n", kLayoutFormatVersion);
    out += std::format("active = {}\n", w.active());

    for (const Workspace& item : w.items()) {
        out += "\n[[workspace]]\n";
        out += std::format("name = {}\n", quote(item.name));
        out += std::format("builtin = {}\n", item.builtin ? "true" : "false");
        out += layout_body(item.layout, "workspace.");
    }
    return out;
}

std::expected<Workspaces, std::string> workspaces_from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("line {}: {}", e.source().begin.line,
                                           std::string_view{e.description()}));
    } catch (const std::exception& e) {
        return std::unexpected(e.what());
    } catch (...) {
        return std::unexpected("the file could not be parsed");
    }

    const auto version = tbl["version"].value<std::int64_t>();
    if (!version)
        return std::unexpected("missing 'version'");
    if (*version != kLayoutFormatVersion)
        return std::unexpected(std::format("unsupported layout format version {} (this build "
                                           "reads version {})",
                                           *version, kLayoutFormatVersion));

    const toml::array* arr = tbl["workspace"].as_array();
    if (!arr || arr->empty()) {
        // The older format: one layout at the top level, no workspaces. Adopt it as Build rather
        // than throwing away an arrangement someone had set up.
        auto single = layout_from_table(tbl);
        if (!single)
            return std::unexpected(single.error());
        Workspaces w = Workspaces::defaults();
        if (const auto r = w.set_layout(0, std::move(*single)); !r)
            return std::unexpected(r.error());
        return w;
    }

    Workspaces w;
    for (std::size_t i = 0; i < arr->size(); ++i) {
        const toml::table* t = arr->get(i)->as_table();
        if (!t)
            return std::unexpected(std::format("workspace #{} is not a table", i));
        const auto name = (*t)["name"].value<std::string>();
        if (!name || name->empty())
            return std::unexpected(std::format("workspace #{} has no 'name'", i));
        auto layout = layout_from_table(*t);
        if (!layout)
            return std::unexpected(std::format("'{}': {}", *name, layout.error()));

        // `builtin` is taken from the name rather than from the file. A hand-edited file that
        // marked every workspace as removable, or marked a new one as built in, would otherwise
        // be able to leave no known arrangement to return to.
        const bool builtin = (*name == Workspaces::kBuild) || (*name == Workspaces::kLive);
        if (builtin) {
            if (const auto r = w.add(*name, std::move(*layout)); !r)
                return std::unexpected(r.error());
            w.make_builtin(w.size() - 1);
        } else if (const auto r = w.add(*name, std::move(*layout)); !r) {
            return std::unexpected(r.error());
        }
    }

    // An out-of-range index in a hand-edited file just leaves the first one active, which is
    // why the result is deliberately ignored rather than treated as a parse failure.
    const auto active = tbl["active"].value<std::int64_t>();
    if (active && *active >= 0)
        static_cast<void>(w.set_active(static_cast<std::size_t>(*active)));

    if (const auto bad = w.validate())
        return std::unexpected(*bad);
    return w;
}

std::expected<void, std::string> save(const Workspaces& w, const std::filesystem::path& path) {
    return lc::write_atomic(path, to_toml(w));
}

WorkspacesLoadOutcome load_workspaces_or_default(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return WorkspacesLoadOutcome{Workspaces::defaults(), {}, true};

    const auto text = lc::read_file(path);
    if (!text)
        return WorkspacesLoadOutcome{Workspaces::defaults(), text.error(), true};

    auto w = workspaces_from_toml(*text);
    if (!w)
        return WorkspacesLoadOutcome{Workspaces::defaults(), w.error(), true};
    return WorkspacesLoadOutcome{std::move(*w), {}, false};
}

std::expected<void, std::string> save(const Layout& l, const std::filesystem::path& path) {
    return lc::write_atomic(path, to_toml(l));
}

std::expected<Layout, std::string> load(const std::filesystem::path& path) {
    const auto text = lc::read_file(path);
    if (!text)
        return std::unexpected(text.error());
    return from_toml(*text);
}

LoadOutcome load_or_default(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return LoadOutcome{Layout::default_studio(), {}, true};

    auto l = load(path);
    if (!l)
        return LoadOutcome{Layout::default_studio(), l.error(), true};
    return LoadOutcome{std::move(*l), {}, false};
}

} // namespace llayout
