// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/migrate.hpp"

#include <format>

namespace ldoc {
namespace {

/// Version 1 -> 2, the bump ADR-0027, ADR-0028 and ADR-0029 share.
///
/// What a version 1 project looks like, and what each part becomes:
///
///  - Scenes had no canvas. Every one is assigned the **first** canvas, which is the only one
///    the shell has ever worked against, so this is a restatement of what was already true
///    rather than a guess (ADR-0027).
///  - Nodes could carry `canvas_overrides`. They are gone with the shared scenes they existed
///    for. The reader counts them on the way past and the caller reports the count; nothing is
///    dropped in silence.
void to_v2(Project& p, std::vector<Problem>& out) {
    if (p.canvases.empty()) {
        // Nothing to assign scenes to. validate() will call every scene an error, which is the
        // right outcome and a more useful message than anything invented here would be.
        if (!p.scenes.empty())
            out.push_back(Problem{Severity::Error,
                                  std::format("this project is from an older format and has {} "
                                              "scene(s) but no canvas to put them on",
                                              p.scenes.size()),
                                  p.id});
        return;
    }

    const Id first = p.canvases.front().id;
    std::size_t assigned = 0;
    for (Scene& s : p.scenes) {
        if (!s.canvas.is_nil())
            continue;
        s.canvas = first;
        ++assigned;
    }
    if (assigned > 0)
        out.push_back(Problem{Severity::Warning,
                              std::format("{} scene(s) from an older format were assigned to the "
                                          "canvas '{}', which is the one they were authored "
                                          "against",
                                          assigned, p.canvases.front().name),
                              p.canvases.front().id});
}

} // namespace

Migration migrate(Project& project) {
    Migration m;
    m.from = project.schema_version;
    if (project.schema_version >= kSchemaVersion)
        return m;

    if (project.schema_version < 2)
        to_v2(project, m.problems);

    project.schema_version = kSchemaVersion;
    return m;
}

} // namespace ldoc
