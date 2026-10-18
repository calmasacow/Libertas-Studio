// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lcomp {

/// A still, straight sRGB8, tightly packed. The same pixel layout as `Picture`.
struct DecodedImage {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

/// PNG, JPEG, or WebP, recognised from the bytes rather than the name.
///
/// Empty when there is nothing to draw: the file is missing, it is not one of those, or it is
/// larger than the preview will allocate. A stand-in would be a picture the operator did not
/// choose.
[[nodiscard]] std::optional<DecodedImage> decode_image(const std::string& path);

} // namespace lcomp
