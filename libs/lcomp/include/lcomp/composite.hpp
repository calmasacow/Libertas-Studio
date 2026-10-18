// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ldoc/document.hpp"

#include <cstdint>
#include <functional>
#include <optional>
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
    /// Skew, or a rotation that is not a quarter turn. A flip is a negative scale, and a
    /// quarter turn is transposed in place: both are the picture that was asked for. Anything
    /// else would be a different one.
    UnsupportedTransform,
    /// An enabled effect would change the pixels, and none are applied here.
    UnsupportedEffect,
    /// A precomp naming a scene the project does not have.
    MissingScene,
    /// A precomp that reaches a scene already on the stack.
    Cycle,
    /// An image whose file is empty, missing, unreadable, not a PNG, JPEG, or WebP, or larger
    /// than the preview will allocate. Nothing is invented in its place.
    Unreadable,
    /// A screen source with no frame yet: capture has not started, or the screen has not been
    /// touched since it did (ADR-0013, capture is damage-driven). Distinct from
    /// `UnsupportedType`, which means this picture cannot draw the thing at all -- here it can,
    /// and there is simply nothing to draw yet. The caption reads differently for a reason: one
    /// is a limit of the preview and the other resolves itself the moment the screen redraws.
    NoFrame,
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
    /// When set, the pixels to read are these rather than `pixels` -- a buffer owned by someone
    /// else for the length of one call. Used for a captured frame, which is 33 MB at 4K and must
    /// not be copied into the walk just to be sampled down to a canvas.
    const std::uint8_t* borrowed = nullptr;

    /// Where to read from: the borrowed buffer when there is one, otherwise our own.
    [[nodiscard]] const std::uint8_t* read_from() const {
        return borrowed != nullptr ? borrowed : pixels.data();
    }
    PictureStatus status = PictureStatus::NoProgram;
    std::vector<Gap> gaps;
};

/// The largest edge this preview will allocate. A corrupt document does not get to ask for
/// a gigabyte, and a canvas past this is reported rather than clamped — clamping would be a
/// different picture than the one the operator authored.
inline constexpr std::int32_t kMaxPreviewEdge = 8192;

/// Where a node is drawn, in canvas pixels, before the integer snap the picture uses.
///
/// The Program canvas sizes its handles from this. A second copy of the fit rules would drift,
/// and the handles would miss the picture. The rectangle is the axis-aligned bounds after a
/// quarter turn and a flip, so the handles sit on the picture. `base_width` is the drawn size
/// before scale. `rel_cx` and `rel_dx` are the pivot and the unscaled origin, relative to the
/// position.
struct DrawnFrame {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double base_width = 0;
    double base_height = 0;
    double rel_cx = 0;
    double rel_cy = 0;
    double rel_dx = 0;
    double rel_dy = 0;
};

/// 0, 1, 2, or 3 clockwise quarter-turns in screen space. Empty when `degrees` is not one of
/// those: a slanted picture is not drawn, rather than drawn upright.
[[nodiscard]] std::optional<int> quarter_turn(double degrees);

/// The same placement the compositor paints. `why` is set when nothing is returned. A frame
/// whose width or height is not positive is a scale that collapsed it; the picture draws nothing.
[[nodiscard]] std::optional<DrawnFrame> drawn_frame(const ldoc::Node& node, double natural_w,
                                                    double natural_h, bool have_natural,
                                                    GapReason& why);

/// Position and scale that put the drawn rectangle on `x, y, width, height`.
///
/// Rotation stays, and so does the sign of the scale, which is the flip. Empty when this
/// picture cannot draw the node's transform, or the rectangle is not usable.
struct SolvedFrame {
    double x = 0;
    double y = 0;
    double scale_x = 1;
    double scale_y = 1;
};

[[nodiscard]] std::optional<SolvedFrame> solve_drawn_rect(const ldoc::Node& node, double natural_w,
                                                          double natural_h, bool have_natural,
                                                          double x, double y, double width,
                                                          double height);

/// A captured frame, as the compositor rendered it and the capture path read it back.
///
/// Straight sRGB8 like `Picture`, because that is what the preview draws and what `blit` samples.
/// Deliberately a plain buffer and not a handle: `lcomp` owns no device (ADR-0005 puts the GPU in
/// `libertas-render`, ADR-0040 kept it out of this library), so whoever has the device reads the
/// frame back and hands the pixels in. That keeps one scene walk rather than a second one on the
/// GPU that has to agree with this one -- which is the disagreement ADR-0040's last consequence
/// warns about.
struct CapturedPicture {
    std::int32_t width = 0;
    std::int32_t height = 0;
    /// `width * height * 4` bytes, **B, G, R, A** -- the order the compositor delivers.
    ///
    /// Borrowed, not owned: it points at the buffer the capture thread last wrote, and is valid
    /// only for the call that was handed it. Owning it meant copying 33 MB per frame at 4K --
    /// 2.6 ms, against 1.6 ms to do the actual scaling -- and then copying it again out of the
    /// session. The scaler reads each pixel it needs straight from here instead.
    const std::uint8_t* pixels = nullptr;
    std::size_t size = 0;

    [[nodiscard]] bool empty() const { return pixels == nullptr || size == 0; }
};

/// Asked for the latest frame of the screen a source names, by connector ("DP-1", or empty for
/// the first screen -- ADR-0041).
///
/// Returning an empty picture is the normal answer, not an error: capture is damage-driven
/// (ADR-0013), so a screen nobody has touched has sent nothing, and a source whose capture has
/// not started yet has nothing either. Both are gaps in the picture, counted in the caption, for
/// the same reason an unreadable image is -- the alternative is inventing a frame.
using FrameSource = std::function<CapturedPicture(const std::string& connector)>;

/// Draw the program scene of `canvas` into a new picture.
///
/// `roots[0]` is the topmost node and is drawn last, which is the order the Sources panel
/// shows. Groups pass their opacity down. A precomp is drawn into its own buffer first, so a
/// cycle is a gap instead of a hang. An image is the file it names, decoded and placed with
/// the same fit as a colour. Audio-only sources are skipped: they have no picture to fail at,
/// and counting them would make a microphone a permanent "not drawn".
[[nodiscard]] Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas);

/// The same walk, with somewhere to get captured frames from.
///
/// A screen source draws when `frames` returns pixels for its connector, and is a gap when it
/// does not. The overload above is this one with no frame source, which is what every caller that
/// has no device wants -- a screen source is then the `UnsupportedType` gap it has always been.
[[nodiscard]] Picture composite_canvas(const ldoc::Project& project, const ldoc::Id& canvas,
                                       const FrameSource& frames);

} // namespace lcomp
