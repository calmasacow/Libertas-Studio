// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ltest.hpp"

#include <cstdlib>
#include <filesystem>
#include <lencode/encoder.hpp>

extern "C" {
#include <libavformat/avformat.h>
}

using namespace libertas::encode;

namespace {
RuntimeConfig configured(const std::string& implementation, const std::string& codec = "h264",
                         const std::string& rate = "cbr") {
    ldoc::Encode video;
    video.encoder.implementation = implementation;
    video.encoder.codec = codec;
    video.encoder.rate_control = rate;
    video.encoder.keyframe_interval_ms = 1000;
    video.encoder.preset = implementation == "x264" ? "fast" : "p5";
    video.encoder.extra = {{"nvenc.tuning", std::string("hq")},
                           {"nvenc.multipass", std::string("quarter")},
                           {"nvenc.lookahead", true},
                           {"nvenc.adaptive_quantization", true},
                           {"nvenc.bframes", std::int64_t{2}},
                           {"gpu", std::int64_t{0}},
                           {"x264.profile", std::string("high")},
                           {"x264.options", std::string("bframes=2 ref=2")}};
    if (rate == "crf") {
        video.encoder.extra["quality"] = std::int64_t{20};
        video.encoder.extra["x264.custom_buffer"] = true;
        video.encoder.extra["x264.buffer_kbps"] = std::int64_t{3000};
    }
    ldoc::AudioEncode audio;
    ldoc::Canvas canvas;
    canvas.width = canvas.height = 256;
    canvas.fps = {30, 1};
    auto cfg = runtime_config(video, audio, canvas, "");
    cfg.audio_channels = 0;
    cfg.path = (std::filesystem::temp_directory_path() /
                ("libertas-options-" + ldoc::Id::generate().to_string() + ".mkv"))
                   .string();
    return cfg;
}

void record(RuntimeConfig cfg, AVCodecID expected_codec, int expected_profile) {
    std::string error;
    auto encoder = encoder_open(cfg, error);
    REQUIRE_MSG(encoder, "{}", error);
    std::vector<std::uint8_t> red(256 * 256 * 4, 0);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = red[i + 3] = 255;
    }
    for (int i = 0; i < 65; ++i)
        REQUIRE_MSG(encoder_video(*encoder, red.data(), 256 * 4,
                                  static_cast<std::int64_t>(i) * 1000000000 / 30, error),
                    "{}", error);
    REQUIRE_MSG(encoder_finish(*encoder, error), "{}", error);
    CHECK_EQ(encoder_stats(*encoder).video_encoder, cfg.force_tier);
    encoder.reset();
    AVFormatContext* file = nullptr;
    REQUIRE(avformat_open_input(&file, cfg.path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(file, nullptr) >= 0);
    const int stream = av_find_best_stream(file, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    CHECK_EQ(file->streams[stream]->codecpar->codec_id, expected_codec);
    CHECK_EQ(file->streams[stream]->codecpar->profile, expected_profile);
    CHECK(file->streams[stream]->codecpar->video_delay > 0);
    AVPacket* packet = av_packet_alloc();
    REQUIRE(packet);
    int keyframes = 0;
    while (av_read_frame(file, packet) >= 0) {
        if (packet->stream_index == stream && (packet->flags & AV_PKT_FLAG_KEY))
            ++keyframes;
        av_packet_unref(packet);
    }
    // One second at 30 fps: frames 0, 30 and 60 must start a GOP.
    CHECK_EQ(keyframes, 3);
    av_packet_free(&packet);
    avformat_close_input(&file);
    std::filesystem::remove(cfg.path);
}
} // namespace

LTEST_CASE(x264_uses_document_profile_gop_and_custom_bframes) {
    record(configured("x264"), AV_CODEC_ID_H264, AV_PROFILE_H264_HIGH);
}

LTEST_CASE(crf_and_custom_buffer_reach_the_codec) {
    const auto cfg = configured("x264", "h264", "crf");
    CHECK_EQ(cfg.video_options.at("crf"), std::string("20"));
    CHECK_EQ(cfg.video_options.at("bufsize"), std::string("3000000"));
    record(cfg, AV_CODEC_ID_H264, AV_PROFILE_H264_HIGH);
}

LTEST_CASE(unknown_codec_options_are_reported_instead_of_ignored) {
    auto cfg = configured("x264");
    cfg.video_options["libertas-invalid-option"] = "1";
    std::string error;
    CHECK(!encoder_open(cfg, error));
    CHECK(error.contains("libertas-invalid-option"));
    CHECK(!std::filesystem::exists(cfg.path));
}

LTEST_CASE(explicit_codec_selection_never_falls_back_to_h264) {
    auto cfg = configured("x264");
    cfg.force_tier = "libertas-missing-encoder";
    std::string error;
    CHECK(!encoder_open(cfg, error));
    CHECK(error.contains("libertas-missing-encoder"));
    CHECK(!std::filesystem::exists(cfg.path));
}

LTEST_CASE(opus_audio_selection_reaches_the_muxer) {
    auto cfg = configured("x264");
    cfg.audio_channels = 2;
    cfg.audio_codec = "opus";
    std::string error;
    auto encoder = encoder_open(cfg, error);
    REQUIRE_MSG(encoder, "{}", error);
    std::vector<std::uint8_t> pixels(256 * 256 * 4, 0);
    std::vector<float> silence(960 * 2, 0.0f);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(encoder_video(*encoder, pixels.data(), 256 * 4, i * 33333333LL, error));
        REQUIRE(encoder_audio(*encoder, silence.data(), 960, i * 33333333LL, error));
    }
    REQUIRE_MSG(encoder_finish(*encoder, error), "{}", error);
    encoder.reset();
    AVFormatContext* file = nullptr;
    REQUIRE(avformat_open_input(&file, cfg.path.c_str(), nullptr, nullptr) >= 0);
    REQUIRE(avformat_find_stream_info(file, nullptr) >= 0);
    const int stream = av_find_best_stream(file, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    REQUIRE(stream >= 0);
    CHECK_EQ(file->streams[stream]->codecpar->codec_id, AV_CODEC_ID_OPUS);
    avformat_close_input(&file);
    std::filesystem::remove(cfg.path);
}

LTEST_CASE(nvenc_h264_and_hevc_use_the_requested_settings) {
    if (!std::getenv("LIBERTAS_TEST_NVENC"))
        return;
    auto h264 = configured("nvenc");
    CHECK_EQ(h264.video_options.at("preset"), std::string("p5"));
    CHECK_EQ(h264.video_options.at("multipass"), std::string("1"));
    CHECK_EQ(h264.video_options.at("rc-lookahead"), std::string("32"));
    CHECK_EQ(h264.video_options.at("spatial-aq"), std::string("1"));
    CHECK_EQ(h264.video_options.at("gpu"), std::string("0"));
    record(h264, AV_CODEC_ID_H264, AV_PROFILE_H264_HIGH);
    record(configured("nvenc", "h264", "cqp"), AV_CODEC_ID_H264, AV_PROFILE_H264_HIGH);
    record(configured("nvenc", "hevc"), AV_CODEC_ID_HEVC, AV_PROFILE_HEVC_MAIN);
}

LTEST_MAIN("encoding options")
