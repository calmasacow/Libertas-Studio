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
#include "vk_import.hpp"

#include <lcore/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <cstdio>

#include <unistd.h>

namespace {

void usage() {
    std::puts(R"(capture-spike - Phase-0 portal/PipeWire/Vulkan capture probe

usage:
  capture-spike [--stage N] [--window] [--no-cursor] [--restore-token TOK]

  --stage N            stop after stage N (1=portal, 2=stream, 3=vulkan import)
  --sweep              try every modifier variant inside ONE portal session (one picker click)
  --drm-node MAJ:MIN   select the Vulkan device by DRM node (never by index)
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

namespace {

/// Closes the portal session on any exit path. Leaking one leaves a live capture stream on the
/// compositor and poisons subsequent sessions (see docs/adr/0009).
struct SessionCloser {
    const std::string* handle = nullptr;
    ~SessionCloser() {
        if (handle != nullptr && !handle->empty())
            libertas::spike::close_screencast_session(*handle);
    }
};

} // namespace

int main(int argc, char** argv) {
    lc::init_from_env();

    int stage = 3; // raised as later stages land
    bool shm_only = false;
    std::vector<std::uint64_t> explicit_mods;
    bool dump_formats = false;
    bool mirror = false;
    bool sweep = false;
    std::string prefer_drm_node;
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
        } else if (a == "--drm-node" && i + 1 < args.size()) {
            prefer_drm_node = std::string(args[++i]);
        } else if (a == "--sweep") {
            sweep = true;
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

    const SessionCloser closer{&session.session_handle};

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

    // Stage 3 brings up Vulkan first, so negotiation can advertise the modifiers the device
    // actually supports instead of a guess. This is what we should have done from the start.
    libertas::spike::VkCtxPtr vk;
    std::vector<std::uint64_t> vk_mods;
    if (stage >= 3 || sweep) {
        std::string verr;
        vk = libertas::spike::vk_init(prefer_drm_node, verr);
        if (!vk) {
            lc::error("vulkan init failed: {}", verr);
            return 4;
        }
        lc::info("vulkan device: {}", libertas::spike::vk_device_name(*vk));
        // ARGB8888 is what a BGRA SPA format maps to.
        vk_mods = libertas::spike::vk_supported_modifiers(*vk, 0x34325241 /* DRM_FORMAT_ARGB8888 */);
        std::printf("\n=== modifiers this device can import for ARGB8888 (%zu) ===\n",
                    vk_mods.size());
        for (std::uint64_t m : vk_mods)
            std::printf("  %s\n", libertas::spike::modifier_str(m).c_str());
        std::printf("\n");
    }

    if (sweep) {
        struct Variant {
            const char* label;
            std::vector<std::uint64_t> mods;
        };
        std::vector<Variant> variants{
            {"shm only (no modifier prop)", {}},
            {"INVALID", {libertas::spike::kModifierInvalid}},
            {"LINEAR", {libertas::spike::kModifierLinear}},
            {"INVALID + LINEAR", {libertas::spike::kModifierInvalid,
                                  libertas::spike::kModifierLinear}},
        };
        if (!vk_mods.empty()) {
            variants.push_back({"Vulkan-supported list", vk_mods});
            std::vector<std::uint64_t> with_invalid = vk_mods;
            with_invalid.insert(with_invalid.begin(), libertas::spike::kModifierInvalid);
            variants.push_back({"INVALID + Vulkan list", with_invalid});
        }

        std::printf("=== sweep: %zu variants against ONE session (node %u, %s) ===\n\n",
                    variants.size(), session.streams.front().node_id,
                    session.streams.front().mapping_id.c_str());

        for (const Variant& v : variants) {
            libertas::spike::StreamConfig vc;
            // Each attempt needs its own fd: pw_context_connect_fd takes ownership.
            vc.pipewire_fd = ::dup(session.pipewire_fd);
            vc.node_id = session.streams.front().node_id;
            vc.max_frames = 5;
            vc.modifiers = v.mods;

            std::uint32_t dma = 0;
            std::uint32_t shm = 0;
            vc.on_frame = [&dma, &shm](const libertas::spike::CapturedFrame& f) {
                if (f.is_dmabuf) ++dma; else ++shm;
            };

            libertas::spike::NegotiatedFormat vf;
            std::string verr2;
            const bool ok = libertas::spike::run_stream(vc, vf, verr2);

            std::printf("  %-26s : ", v.label);
            if (!ok) {
                std::printf("FAIL  (%s)\n", verr2.c_str());
            } else {
                std::printf("OK  %ux%u %s mod=%-34s %u dmabuf %u shm\n", vf.width, vf.height,
                            libertas::spike::fourcc_str(vf.drm_format).c_str(),
                            vf.have_modifier
                                ? libertas::spike::modifier_str(vf.modifier).c_str()
                                : "none",
                            dma, shm);
            }
            std::fflush(stdout);
        }
        std::printf("\n");
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
        if (!explicit_mods.empty())
            sc.modifiers = explicit_mods;
        else if (!vk_mods.empty())
            sc.modifiers = vk_mods; // the device's real capability
        else
            sc.modifiers = {libertas::spike::kModifierInvalid, libertas::spike::kModifierLinear};
    }

    struct Stats {
        std::uint32_t dmabuf = 0;
        std::uint32_t shm = 0;
        bool printed_detail = false;
        std::uint32_t imported = 0;
        std::uint32_t import_failed = 0;
        std::string first_import_error;
        std::uint64_t checksum = 0;
        std::uint32_t nonzero_pixels = 0;
        std::vector<std::uint8_t> first_image;
        std::uint32_t img_w = 0;
        std::uint32_t img_h = 0;
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

    // Chain Vulkan import onto the frame callback when stage 3 is active.
    if (stage >= 3 && vk) {
        auto base = sc.on_frame;
        libertas::spike::VkCtx* vkp = vk.get();
        sc.on_frame = [&stats, base, vkp](const libertas::spike::CapturedFrame& f) {
            if (base)
                base(f);
            if (!f.is_dmabuf)
                return;
            // Import only the first few frames: this is a correctness proof, not a benchmark.
            if (stats.imported + stats.import_failed >= 3)
                return;
            std::vector<std::uint8_t> pixels;
            std::string ierr;
            if (!libertas::spike::vk_import_and_readback(*vkp, f, pixels, ierr)) {
                ++stats.import_failed;
                if (stats.first_import_error.empty())
                    stats.first_import_error = ierr;
                return;
            }
            ++stats.imported;
            if (stats.first_image.empty()) {
                stats.first_image = pixels;
                stats.img_w = f.width;
                stats.img_h = f.height;
                for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
                    stats.checksum = stats.checksum * 1099511628211ULL ^ pixels[i];
                    if ((pixels[i] | pixels[i + 1] | pixels[i + 2]) != 0)
                        ++stats.nonzero_pixels;
                }
            }
        };
    }

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

    // ---------------------------------------------------------------- stage 3: Vulkan import
    std::printf("=== stage 3 result: Vulkan dmabuf import ===\n");
    std::printf("  device        : %s\n", libertas::spike::vk_device_name(*vk).c_str());
    std::printf("  imported      : %u ok, %u failed\n", stats.imported, stats.import_failed);
    if (!stats.first_import_error.empty())
        std::printf("  first error   : %s\n", stats.first_import_error.c_str());
    if (stats.imported > 0) {
        const std::uint32_t total = stats.img_w * stats.img_h;
        std::printf("  readback      : %zu bytes (%ux%u)\n", stats.first_image.size(),
                    stats.img_w, stats.img_h);
        std::printf("  non-black px  : %u / %u (%.1f%%)\n", stats.nonzero_pixels, total,
                    total > 0 ? 100.0 * stats.nonzero_pixels / total : 0.0);
        std::printf("  fnv checksum  : 0x%016lx\n", stats.checksum);

        // Write a PPM so the frame can be eyeballed - the only honest proof of correctness.
        if (const char* out_path = "/tmp/capture-spike-frame.ppm"; stats.img_w > 0) {
            if (std::FILE* fp = std::fopen(out_path, "wb"); fp != nullptr) {
                std::fprintf(fp, "P6\n%u %u\n255\n", stats.img_w, stats.img_h);
                for (std::size_t i = 0; i + 3 < stats.first_image.size(); i += 4) {
                    // readback is B,G,R,A in memory for VK_FORMAT_B8G8R8A8_UNORM
                    const std::uint8_t rgb[3] = {stats.first_image[i + 2],
                                                 stats.first_image[i + 1],
                                                 stats.first_image[i]};
                    std::fwrite(rgb, 1, 3, fp);
                }
                std::fclose(fp);
                std::printf("  wrote         : %s\n", out_path);
            }
        }
    }
    std::printf("\n");

    const bool pass = stats.imported > 0 && stats.nonzero_pixels > 0;
    std::printf("  VERDICT: %s\n\n",
                pass ? "PASS - dmabuf imported into Vulkan with real pixel content"
                     : "FAIL - see errors above");
    return pass ? 0 : 5;
}
