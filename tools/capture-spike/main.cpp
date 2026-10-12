// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phase-0 capture spike. Staged deliberately so each stage is verifiable on its own:
//   stage 1  portal  : negotiate a ScreenCast session, print node id + PipeWire fd
//   stage 2  stream  : connect to the node, report the negotiated format/modifier, count frames
//   stage 3  import  : import each dmabuf into a VkImage and prove correctness by readback
// Run with --stage N (default: the highest implemented stage).

#include "portal.hpp"
#include "stream.hpp"

#include <lcore/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

void usage() {
    std::puts(R"(capture-spike - Phase-0 portal/PipeWire/Vulkan capture probe

usage:
  capture-spike [--stage N] [--window] [--no-cursor] [--restore-token TOK]

  --stage N            stop after stage N (1=portal, 2=stream, 3=vulkan import)
  --mirror             advertise one fully-fixed format matching the source (bisect helper)
  --dump-formats       print the EnumFormat params the source node offers, then exit
  --shm-only           advertise no modifiers (shm path only) - for bisecting negotiation
  --modifiers LIST     comma-separated DRM modifiers to advertise, e.g. 0x0,0x300000000e08014
  --window             request window sources instead of monitors
  --no-cursor          request a hidden cursor instead of embedded
  --restore-token TOK  reuse a previous session and skip the picker

A share picker will appear unless a valid --restore-token is given.)");
}

const char* source_type_name(std::uint32_t t) {
    switch (t) {
    case 1: return "monitor";
    case 2: return "window";
    case 4: return "virtual";
    default: return "unknown";
    }
}

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();

    int stage = 2; // raised as later stages land
    bool shm_only = false;
    std::vector<std::uint64_t> explicit_mods;
    bool dump_formats = false;
    bool mirror = false;
    libertas::spike::PortalRequest req;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view a = args[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--stage" && i + 1 < args.size()) {
            stage = std::atoi(std::string(args[++i]).c_str());
        } else if (a == "--window") {
            req.source_types = static_cast<std::uint32_t>(libertas::spike::SourceType::Window);
        } else if (a == "--no-cursor") {
            req.cursor_mode = libertas::spike::CursorMode::Hidden;
        } else if (a == "--modifiers" && i + 1 < args.size()) {
            // Comma-separated, hex (0x..) or decimal. Lets us probe what a compositor accepts
            // without a rebuild.
            const std::string list(args[++i]);
            std::size_t pos = 0;
            while (pos <= list.size()) {
                const std::size_t comma = list.find(',', pos);
                const std::string tok = list.substr(pos, comma == std::string::npos
                                                            ? std::string::npos
                                                            : comma - pos);
                if (!tok.empty())
                    explicit_mods.push_back(std::strtoull(tok.c_str(), nullptr, 0));
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
            }
        } else if (a == "--mirror") {
            mirror = true;
        } else if (a == "--dump-formats") {
            dump_formats = true;
        } else if (a == "--shm-only") {
            shm_only = true;
        } else if (a == "--restore-token" && i + 1 < args.size()) {
            req.restore_token = std::string(args[++i]);
        } else {
            lc::error("unknown argument '{}' (try --help)", a);
            return 1;
        }
    }

    // ---------------------------------------------------------------- stage 1: portal
    lc::info("stage 1: negotiating portal ScreenCast session");
    libertas::spike::PortalSession session;
    std::string error;
    if (!libertas::spike::negotiate_screencast(req, session, error)) {
        lc::error("portal negotiation failed: {}", error);
        return 2;
    }

    std::printf("\n=== portal session ===\n");
    std::printf("  pipewire fd   : %d\n", session.pipewire_fd);
    std::printf("  streams       : %zu\n", session.streams.size());
    for (const auto& s : session.streams) {
        std::printf("    node %u  %dx%d  at (%d,%d)  type=%s%s%s\n", s.node_id, s.width, s.height,
                    s.pos_x, s.pos_y, source_type_name(s.source_type),
                    s.mapping_id.empty() ? "" : "  mapping_id=", s.mapping_id.c_str());
    }
    if (!session.restore_token.empty())
        std::printf("  restore token : %s\n     (reuse with --restore-token to skip the picker)\n",
                    session.restore_token.c_str());
    else
        std::printf("  restore token : (none returned)\n");
    std::printf("\n");

    if (stage <= 1) {
        lc::info("stage 1 complete");
        if (session.pipewire_fd >= 0)
            ::close(session.pipewire_fd);
        return 0;
    }

    if (dump_formats) {
        std::string derr;
        if (!libertas::spike::dump_node_formats(session.pipewire_fd,
                                                session.streams.front().node_id, derr))
            lc::warn("format dump produced nothing: {}", derr.empty() ? "no params" : derr);
        return 0;
    }

    // ---------------------------------------------------------------- stage 2: PipeWire stream
    lc::info("stage 2: connecting PipeWire stream to node {}", session.streams.front().node_id);

    libertas::spike::StreamConfig sc;
    sc.pipewire_fd = session.pipewire_fd; // ownership moves to the stream
    sc.node_id = session.streams.front().node_id;
    sc.max_frames = 30;
    if (mirror) {
        sc.mirror_width = static_cast<std::uint32_t>(session.streams.front().width);
        sc.mirror_height = static_cast<std::uint32_t>(session.streams.front().height);
    }
    // Stage 2 advertises a broad list to learn what the compositor actually offers. Stage 3
    // replaces this with the set Vulkan reports as importable for the chosen format.
    if (!shm_only) {
        sc.modifiers = explicit_mods.empty()
                           ? std::vector<std::uint64_t>{libertas::spike::kModifierInvalid,
                                                        libertas::spike::kModifierLinear}
                           : explicit_mods;
    }

    struct Stats {
        std::uint32_t dmabuf = 0;
        std::uint32_t shm = 0;
        bool printed_detail = false;
    } stats;

    sc.on_frame = [&stats](const libertas::spike::CapturedFrame& f) {
        if (f.is_dmabuf) ++stats.dmabuf; else ++stats.shm;
        if (!stats.printed_detail) {
            stats.printed_detail = true;
            std::printf("\n=== first frame ===\n");
            std::printf("  %ux%u  drm_format=%s  modifier=%s\n", f.width, f.height,
                        libertas::spike::fourcc_str(f.drm_format).c_str(),
                        libertas::spike::modifier_str(f.modifier).c_str());
            std::printf("  buffer type   : %s\n", f.is_dmabuf ? "DmaBuf" : "shm (MemPtr/MemFd)");
            std::printf("  planes        : %zu\n", f.planes.size());
            for (std::size_t i = 0; i < f.planes.size(); ++i) {
                const auto& p = f.planes[i];
                std::printf("    [%zu] fd=%-4d offset=%-8u stride=%-8u maxsize=%ld\n", i, p.fd,
                            p.offset, p.stride, static_cast<long>(p.size));
            }
            std::printf("\n");
        }
    };

    libertas::spike::NegotiatedFormat fmt;
    std::string serr;
    if (!libertas::spike::run_stream(sc, fmt, serr)) {
        lc::error("stream failed: {}", serr);
        return 3;
    }

    std::printf("=== stage 2 result ===\n");
    std::printf("  frames        : %u dmabuf, %u shm\n", stats.dmabuf, stats.shm);
    std::printf("  negotiated    : %ux%u  drm=%s  modifier=%s\n", fmt.width, fmt.height,
                libertas::spike::fourcc_str(fmt.drm_format).c_str(),
                fmt.have_modifier ? libertas::spike::modifier_str(fmt.modifier).c_str()
                                  : "none (shm)");
    std::printf("  zero-copy path: %s\n\n",
                stats.dmabuf > 0 ? "AVAILABLE (dmabuf frames received)"
                                 : "NOT TAKEN (shm fallback only)");

    if (stage <= 2) {
        lc::info("stage 2 complete");
        return 0;
    }

    lc::warn("stage {} not implemented yet", stage);
    return 0;
}
