// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libertas::spike {

/// Which encoder tier was actually used. ADR-0006 specifies a ladder rather than one encoder,
/// because the top rung is not available everywhere and failing to start a show is not an option.
enum class VideoTier {
    None,
    Vulkan,   ///< h264_vulkan on OUR VkDevice - no interop in the hot path
    Nvenc,    ///< h264_nvenc, software frames (costs an upload)
    X264,     ///< libx264 - the floor, always present
};

const char* tier_name(VideoTier t);

struct EncoderConfig {
    std::string path;              ///< output file; the container is chosen from its extension
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t fps_num = 60;
    std::int32_t fps_den = 1;
    std::int64_t bitrate = 12'000'000;
    std::string force_tier;        ///< "vulkan"|"nvenc"|"x264"; empty = walk the ladder

    /// Audio. Zero channels disables the audio stream entirely, which is useful for isolating a
    /// video problem but is NOT the Phase-0 gate - the gate requires synced audio.
    std::uint32_t audio_rate = 48000;
    std::uint32_t audio_channels = 2;
    std::int64_t audio_bitrate = 160'000;
};

struct EncoderStats {
    VideoTier tier = VideoTier::None;
    std::string video_encoder;
    std::string audio_encoder;
    std::uint64_t video_frames = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t video_bytes = 0;
    std::uint64_t audio_bytes = 0;
    /// Wall time spent inside avcodec_send_frame/receive_packet, which is the number that says
    /// whether this encoder can hold the show's frame budget.
    std::vector<double> video_encode_ms;
    std::vector<double> convert_ms;
};

struct Encoder;
void encoder_close(Encoder*);
struct EncoderDeleter {
    void operator()(Encoder* p) const { encoder_close(p); }
};
using EncoderPtr = std::unique_ptr<Encoder, EncoderDeleter>;

/// Opens the output file and brings up both streams. Walks the encoder ladder unless pinned.
EncoderPtr encoder_open(const EncoderConfig& cfg, std::string& error);

/// Submits one BGRA frame. `pts_ns` is on the SAME clock as audio_submit's - see ADR-0017: the
/// muxer does not care which clock, only that there is exactly one.
bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error);

/// Submits interleaved float32 audio. `frames` is sample-frames, not bytes.
bool encoder_audio(Encoder& enc, const float* interleaved, std::uint32_t frames,
                   std::int64_t pts_ns, std::string& error);

/// Flushes both encoders and writes the trailer. A file is only playable after this.
bool encoder_finish(Encoder& enc, std::string& error);

const EncoderStats& encoder_stats(Encoder& enc);

} // namespace libertas::spike
