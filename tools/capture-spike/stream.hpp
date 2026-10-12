// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace libertas::spike {

struct DmabufPlane {
    int fd = -1;
    std::uint32_t offset = 0;
    std::uint32_t stride = 0;
    std::int64_t size = 0;
};

/// One frame as PipeWire delivered it. Plane fds are borrowed; valid only during the callback.
struct CapturedFrame {
    std::uint64_t sequence = 0;
    std::int64_t pts = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t drm_format = 0;   ///< DRM_FORMAT_* fourcc
    std::uint64_t modifier = 0;     ///< DRM format modifier, or kModifierInvalid
    bool is_dmabuf = false;         ///< false => shm/MemPtr fallback path
    std::vector<DmabufPlane> planes;
};

struct NegotiatedFormat {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t spa_format = 0;
    std::uint32_t drm_format = 0;
    std::uint64_t modifier = 0;
    bool have_modifier = false;
    std::int32_t fps_num = 0;
    std::int32_t fps_den = 1;
};

inline constexpr std::uint64_t kModifierInvalid = 0x00ffffffffffffffULL;
inline constexpr std::uint64_t kModifierLinear = 0;

struct StreamConfig {
    int pipewire_fd = -1;       ///< from the portal; the stream takes ownership
    std::uint32_t node_id = 0;
    /// Modifiers to advertise, most preferred first. Empty => shm only.
    std::vector<std::uint64_t> modifiers;
    /// Diagnostic: when non-zero, advertise a single fully-fixed format at this size instead of
    /// negotiable choices, to bisect which property a compositor rejects.
    std::uint32_t mirror_width = 0;
    std::uint32_t mirror_height = 0;
    std::uint32_t max_frames = 30;  ///< stop after this many
    /// Called per frame on the PipeWire thread.
    std::function<void(const CapturedFrame&)> on_frame;
};

/// Connects to the node, negotiates a format, pumps frames until max_frames or error.
/// Returns false and fills `error` on failure. `out_format` reflects what was actually agreed.
bool run_stream(const StreamConfig& cfg, NegotiatedFormat& out_format, std::string& error);

/// Asks the source node what EnumFormat params it offers and prints them. Diagnostic: this is how
/// to learn which DRM format modifiers a compositor will actually give us, rather than guessing.
bool dump_node_formats(int pipewire_fd, std::uint32_t node_id, std::string& error);

/// Human-readable fourcc, e.g. "AR24".
std::string fourcc_str(std::uint32_t fourcc);
/// Decodes the vendor nibble of a DRM format modifier, e.g. "NVIDIA(0x...)".
std::string modifier_str(std::uint64_t modifier);

} // namespace libertas::spike
