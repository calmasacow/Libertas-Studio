// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ldoc/migrate.hpp"

#include <algorithm>
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
///  - Outputs carried a canvas and encoder settings of their own. Each distinct pair becomes
///    one `Encode` and every output asking for that same pair points at it (ADR-0029). Sharing
///    rather than one encode per output, because the two are indistinguishable in the document
///    and only one of them is true about the GPU: three outputs that were configured
///    identically really were three encodes before, and really are one now.
///
/// What an encode is called. "1080p60 8 Mbps", built from the canvas and the bitrate -- never
/// the output's name. An encode called "Twitch 1080p" that keeps running after Twitch stops
/// reads as a bug rather than as the feature it is, and that is precisely the state this change
/// makes reachable.
std::string encode_name(const Project& p, const Encode& e) {
    const Canvas* c = p.canvas(e.canvas);
    const double mbps = static_cast<double>(e.encoder.bitrate_kbps) / 1000.0;
    const std::string rate = mbps >= 10.0 || mbps == static_cast<double>(static_cast<int>(mbps))
                                 ? std::format("{:.0f}", mbps)
                                 : std::format("{:.1f}", mbps);
    if (!c)
        return std::format("{} Mbps", rate);
    const double fps = c->fps.den == 0 ? 0.0
                                       : static_cast<double>(c->fps.num) /
                                             static_cast<double>(c->fps.den);
    const std::int32_t height = e.rescale ? static_cast<std::int32_t>(e.rescale->y) : c->height;
    return std::format("{}p{:.0f} {} Mbps", height, fps, rate);
}

/// Outputs that asked for the same canvas and the same encoder settings become one encode.
void outputs_to_encodes(Project& p, const std::vector<LegacyOutputEncode>& legacy,
                        std::vector<Problem>& out) {
    if (legacy.empty())
        return;

    struct Made {
        Id canvas;
        EncoderConfig encoder;
        Id encode;
    };
    std::vector<Made> made;

    for (const LegacyOutputEncode& old : legacy) {
        const auto it = std::find_if(made.begin(), made.end(), [&old](const Made& m) {
            return m.canvas == old.canvas && m.encoder == old.encoder;
        });
        Id encode_id;
        if (it != made.end()) {
            encode_id = it->encode;
        } else {
            Encode e;
            e.id = Id::generate();
            e.canvas = old.canvas;
            e.encoder = old.encoder;
            e.name = encode_name(p, e);
            encode_id = e.id;
            made.push_back(Made{old.canvas, old.encoder, e.id});
            p.encodes.push_back(std::move(e));
        }
        for (Output& o : p.outputs) {
            if (o.id == old.output)
                o.encode = encode_id;
        }
    }

    out.push_back(Problem{Severity::Warning,
                          std::format("{} output(s) from an older format now share {} encode(s); "
                                      "an encode is a document object of its own and outputs "
                                      "name one",
                                      legacy.size(), p.encodes.size()),
                          p.id});
}

void to_v2(Project& p, const std::vector<LegacyOutputEncode>& legacy,
           std::vector<Problem>& out) {
    outputs_to_encodes(p, legacy, out);

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

Migration migrate(Project& project, const std::vector<LegacyOutputEncode>& legacy_outputs) {
    Migration m;
    m.from = project.schema_version;
    if (project.schema_version >= kSchemaVersion)
        return m;

    if (project.schema_version < 2)
        to_v2(project, legacy_outputs, m.problems);

    project.schema_version = kSchemaVersion;
    return m;
}

} // namespace ldoc
