// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace lcomp {

/// Why a node contributed no pixels.
///
/// A gap is not drawn. The picture underneath shows through, and the count is what the Program
/// panel says. Approximating any of these — a wrong blend, an unrotated stand-in, a card where
/// a camera will be — is the lie the panel was built to refuse (ADR-0040).
enum class GapReason : std::uint8_t {
    /// Bound to nothing, or to a source the project does not have.
    NoSource,
    /// A colour source with no colour stored. The shell writes the default; a missing one is
    /// not invented here.
    NoColour,
    /// No box and no natural size, so there is nowhere to put it.
    NoSize,
    /// A type this composite does not draw: capture, media, text, browser, camera.
    UnsupportedType,
    /// Any blend other than Normal, including clipping to the node below.
    UnsupportedBlend,
    /// Rotation, skew, or a negative scale. Axis-aligned, or not at all.
    UnsupportedTransform,
    /// An enabled effect would change the pixels, and none are applied here.
    UnsupportedEffect,
    /// A precomp naming a scene the project does not have.
    MissingScene,
    /// A precomp that reaches a scene already on the stack.
    Cycle,
};

/// What the picture is, as a whole, rather than node by node.
enum class PictureStatus : std::uint8_t {
    /// The canvas has no program scene. The buffer is black.
    NoProgram,
    /// A program scene in which nothing visible could contribute a pixel. Black.
    Empty,
    /// Some nodes drew and some were refused. `gaps` says which.
    Partial,
    /// Every visible node that has a picture was drawn.
    Complete,
    /// The canvas is too large, or not a canvas. `pixels` is empty; nothing was invented.
    Unsupported,
};

struct Gap {
    ldoc::Id node;
    std::string name;
    GapReason reason = GapReason::UnsupportedType;
};

/// One canvas, straight sRGB8, tightly packed, top-left origin.
///
/// Not the encode target. ADR-0006 still composites in scene-linear RGBA16F on a VkDevice;
/// this is the view the Program panel shows, and it is the oracle the tests read. Opaque black
/// where nothing was drawn, because that is what would go to air.
struct Picture {
    std::int32_t width = 0;
    std::int32_t height = 0;
    /// Bytes from one row to the next. Width times 4; there is no padding to discover.
    std::uint32_t stride = 0;
    /// `stride * height` bytes, R, G, B, A in that order. Empty when `status` is Unsupported.
    std::vector<std::uint8_t> pixels;
    PictureStatus status = PictureStatus::NoProgram;
    std::vector<Gap> gaps;
};

/// The largest edge this preview will allocate. A corrupt document does not get to ask for
/// a gigabyte, and a canvas past this is reported rather than clamped — clamping would be a
/// different picture than the one the operator authored.
inline constexpr std::int32_t kMaxPreviewEdge = 8192;

/// Draw the program scene of `canvas` into a new picture.
///
/// `roots[0]` is the topmost node and is drawn last, which is the order the Sources panel
/// shows. Groups pass their opacity down. A precomp is drawn into its own buffer first, so a
/// cycle is a gap instead of a hang. Audio-only sources are skipped: they have no picture to
/// fail at, and counting them would make a microphone a permanent "not drawn".
[[nodiscard]] Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas);

} // namespace lcomp
