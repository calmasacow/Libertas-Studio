// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
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

/// A long-lived PipeWire connection. One per process: pw_init/pw_deinit are not reliably
/// re-entrant, and the fd from OpenPipeWireRemote is effectively single-use, so a sweep must reuse
/// one connection and create/destroy only streams inside it. This is also the shape the real
/// capture worker needs, since it holds a persistent connection across reconfigurations.
struct PwSession;
void pw_session_close(PwSession*);
struct PwSessionDeleter {
    void operator()(PwSession* p) const { pw_session_close(p); }
};
using PwSessionPtr = std::unique_ptr<PwSession, PwSessionDeleter>;

/// Takes ownership of `pipewire_fd`.
PwSessionPtr pw_session_open(int pipewire_fd, std::string& error);

struct StreamConfig {
    int pipewire_fd = -1;       ///< single-shot run_stream only; ignored by pw_try_variant
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

/// Runs one negotiation attempt inside an existing session. `timeout_ms` bounds how long to wait
/// for a format before giving up, so a variant that cannot negotiate does not hang the sweep.
bool pw_try_variant(PwSession& session, const StreamConfig& cfg, NegotiatedFormat& out_format,
                    std::string& error, int timeout_ms = 4000);

/// Connects to the node, negotiates a format, pumps frames until max_frames or error.
/// Returns false and fills `error` on failure. `out_format` reflects what was actually agreed.
bool run_stream(const StreamConfig& cfg, NegotiatedFormat& out_format, std::string& error);

/// Waits until `node_id` advertises at least one SPA_PARAM_EnumFormat.
///
/// portal ScreenCast.Start returns a node id BEFORE the node has populated its format params.
/// Connecting immediately leaves the server nothing to intersect against, which surfaces as
/// "no more input formats" and looks exactly like a modifier incompatibility. Every capture
/// consumer must do this wait; see docs/adr/0009.
/// Returns false on timeout.
bool pw_wait_node_ready(PwSession& session, std::uint32_t node_id, int timeout_ms = 5000);

/// Same as dump_node_formats but over an already-open session, so it can be called at the moment
/// of a negotiation failure. The portal fd is single-use, so this is the only way to see what the
/// source was offering during a live session.
bool pw_dump_formats(PwSession& session, std::uint32_t node_id);

/// Asks the source node what EnumFormat params it offers and prints them. Diagnostic: this is how
/// to learn which DRM format modifiers a compositor will actually give us, rather than guessing.
bool dump_node_formats(int pipewire_fd, std::uint32_t node_id, std::string& error);

/// Human-readable fourcc, e.g. "AR24".
std::string fourcc_str(std::uint32_t fourcc);
/// Decodes the vendor nibble of a DRM format modifier, e.g. "NVIDIA(0x...)".
std::string modifier_str(std::uint64_t modifier);

} // namespace libertas::spike
