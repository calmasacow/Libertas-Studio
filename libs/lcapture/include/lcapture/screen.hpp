// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lcapture/wlcap.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libertas::rhi {
struct VkCtx;
}

namespace libertas::capture {

/// Captures one screen and hands back the last frame as CPU pixels.
///
/// This is the sequence `capture-spike` proved, with the measuring taken out: open the
/// compositor connection, start a session on the named output, intersect the compositor's
/// modifiers with the device's, allocate a ring of buffers ourselves, let the compositor render
/// into them, read the newest one back.
///
/// Readback rather than a GPU handle because the preview compositor is CPU-side and must stay
/// one scene walk (ADR-0040). That costs a copy per frame and is the right trade for a preview;
/// the encode path keeps its frames on the device and does not come through here.
///
/// A ring rather than one buffer: with a single destination the compositor cannot draw frame N+1
/// while we hold its only target, which serialises it against us.
class ScreenCapture {
public:
    /// Opens a capture session for `connector` ("DP-1", or empty for the first output --
    /// ADR-0041), allocating on `device`.
    ///
    /// Returns nothing with `error` set when the compositor has no capture protocol, the named
    /// output is not there, or no modifier is common to the compositor and the device. All three
    /// are things the caller reports rather than retries.
    static std::unique_ptr<ScreenCapture> open(rhi::VkCtx& device, const std::string& connector,
                                               bool with_cursor, std::string& error);

    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    /// Width and height the compositor will deliver, which is the screen's own size.
    [[nodiscard]] std::uint32_t width() const;
    [[nodiscard]] std::uint32_t height() const;

    /// Waits up to `timeout_ms` for a frame and reads it back as RGBA8.
    ///
    /// Returns false and leaves `out` alone when no frame arrived. That is the ordinary case on a
    /// screen nobody is touching -- capture is damage-driven (ADR-0013) -- and `idle` says so, to
    /// distinguish it from a real fault without parsing `error`.
    ///
    /// A timeout is not a way to poll: ADR-0013 records that abandoning a pending frame and asking
    /// again makes the compositor answer at once with unchanged content, which invents a frame per
    /// timeout that the screen never drew. A caller wanting the next real frame waits again.
    bool read_frame(std::vector<std::uint8_t>& out, std::uint32_t timeout_ms, bool& idle,
                    std::string& error);

private:
    ScreenCapture();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace libertas::capture
