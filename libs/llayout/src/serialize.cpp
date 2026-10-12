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
    out += std::format("root = {}\n", l.root());

    for (const NodeId id : l.node_ids()) {
        const Node* n = l.find(id);
        if (!n)
            continue;
        out += "\n[[node]]\n";
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

std::expected<Layout, std::string> from_toml(std::string_view text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        return std::unexpected(std::format("line {}: {}", e.source().begin.line,
                                           std::string_view{e.description()}));
    }

    const auto version = tbl["version"].value<std::int64_t>();
    if (!version)
        return std::unexpected("missing 'version'");
    if (*version != kLayoutFormatVersion)
        return std::unexpected(std::format("unsupported layout format version {} (this build "
                                           "reads version {})",
                                           *version, kLayoutFormatVersion));

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
