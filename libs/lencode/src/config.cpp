// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The join between what the document says and what the muxer needs. See runtime_config in
// encoder.hpp for why it lives here rather than in ldoc.

#include "lencode/encoder.hpp"

#include <utility>

namespace libertas::encode {

RuntimeConfig runtime_config(const ldoc::Encode& encode, const ldoc::AudioEncode& audio,
                             const ldoc::Canvas& canvas, std::string path) {
    RuntimeConfig cfg;
    cfg.path = std::move(path);

    // A rescale says how big the picture is; the canvas always says how often it arrives.
    const double width = encode.rescale ? encode.rescale->x : canvas.width;
    const double height = encode.rescale ? encode.rescale->y : canvas.height;
    cfg.width = static_cast<std::uint32_t>(width > 0 ? width : canvas.width);
    cfg.height = static_cast<std::uint32_t>(height > 0 ? height : canvas.height);
    cfg.source_width = static_cast<std::uint32_t>(canvas.width);
    cfg.source_height = static_cast<std::uint32_t>(canvas.height);
    cfg.source_rgba = true;

    // Exact, never a double: 59.94 is 60000/1001 and a pipeline that rounds it drifts into
    // audio sync hours later.
    cfg.fps_num = canvas.fps.num > 0 ? canvas.fps.num : 60;
    cfg.fps_den = canvas.fps.den > 0 ? canvas.fps.den : 1;

    cfg.bitrate = static_cast<std::int64_t>(encode.encoder.bitrate_kbps) * 1000;

    // The document's `implementation` is what the operator asked for; the ladder decides what
    // is actually available (ADR-0006). An implementation this build has no rung for leaves
    // the tier empty, which means "walk the ladder" rather than "fail".
    if (encode.encoder.implementation == "nvenc")
        cfg.force_tier = tier_name(VideoTier::Nvenc);
    else if (encode.encoder.implementation == "x264")
        cfg.force_tier = tier_name(VideoTier::X264);

    cfg.audio_bitrate = static_cast<std::int64_t>(audio.bitrate_kbps) * 1000;
    return cfg;
}

} // namespace libertas::encode
