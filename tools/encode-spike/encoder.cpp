// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Encode and mux for the Phase-0 gate.
//
// Two things here are deliberate and load-bearing, and both come from earlier ADRs:
//
//  1. Colour conversion is configured, never inferred (ADR-0014). The capture side proved a device
//     will declare a range it does not deliver, so the matrix and range used to turn RGB into YUV
//     are stated explicitly and written into the bitstream, rather than left to libswscale's
//     defaults - which vary with resolution and would silently change the picture.
//
//  2. Audio carries the clock. Its presentation timestamps come from its own sample counter, which
//     is exact by construction, and video is placed against that same origin. Wall-clock arrival
//     time is used for neither.

#include "encoder.hpp"

#include <lcore/log.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace libertas::spike {

const char* tier_name(VideoTier t) {
    switch (t) {
    case VideoTier::Vulkan: return "h264_vulkan";
    case VideoTier::Nvenc: return "h264_nvenc";
    case VideoTier::X264: return "libx264";
    default: return "none";
    }
}

struct Encoder {
    EncoderConfig cfg;
    EncoderStats stats;

    AVFormatContext* fmt = nullptr;

    AVCodecContext* vctx = nullptr;
    AVStream* vst = nullptr;
    AVFrame* vframe = nullptr;
    SwsContext* sws = nullptr;

    AVCodecContext* actx = nullptr;
    AVStream* ast = nullptr;
    AVFrame* aframe = nullptr;
    SwrContext* swr = nullptr;
    AVAudioFifo* fifo = nullptr;
    std::int64_t audio_samples = 0; ///< the master clock, in samples

    AVPacket* pkt = nullptr;

    /// The shared time origin, set by whichever stream submits first. Without one, the two streams
    /// start at different zeroes and the file plays with a fixed offset that looks like drift.
    std::int64_t t0_ns = AV_NOPTS_VALUE;
    bool header_written = false;
};

namespace {

std::string av_err(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof b);
    return b;
}

/// Drains packets from an encoder into the muxer.
bool drain(Encoder& enc, AVCodecContext* ctx, AVStream* st, std::uint64_t& bytes,
           std::string& error) {
    for (;;) {
        const int r = avcodec_receive_packet(ctx, enc.pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return true;
        if (r < 0) {
            error = std::format("avcodec_receive_packet: {}", av_err(r));
            return false;
        }
        enc.pkt->stream_index = st->index;
        av_packet_rescale_ts(enc.pkt, ctx->time_base, st->time_base);
        bytes += static_cast<std::uint64_t>(enc.pkt->size);
        const int w = av_interleaved_write_frame(enc.fmt, enc.pkt);
        av_packet_unref(enc.pkt);
        if (w < 0) {
            error = std::format("av_interleaved_write_frame: {}", av_err(w));
            return false;
        }
    }
}

/// Tries one video encoder by name. Returns nullptr when the encoder is absent or refuses to open,
/// which is the ladder's whole mechanism - "not available" and "present but unusable here" must
/// both fall through to the next rung rather than abort the run.
AVCodecContext* try_video(const char* name, const EncoderConfig& cfg, AVFormatContext* fmt,
                          std::string& why) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name);
    if (codec == nullptr) {
        why = "not built into this libavcodec";
        return nullptr;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    if (c == nullptr) {
        why = "avcodec_alloc_context3 failed";
        return nullptr;
    }
    c->width = static_cast<int>(cfg.width);
    c->height = static_cast<int>(cfg.height);
    c->time_base = AVRational{cfg.fps_den, cfg.fps_num};
    c->framerate = AVRational{cfg.fps_num, cfg.fps_den};
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->bit_rate = cfg.bitrate;
    // A two-second GOP, which is what every live destination wants and what the operator's
    // existing profile already uses (keyint 2).
    c->gop_size = cfg.fps_num / cfg.fps_den * 2;
    c->max_b_frames = 0; // live: no reordering delay
    // State the colour properties rather than leaving them unset. An unset matrix in an H.264
    // bitstream means "assume BT.601", which is wrong for everything we produce.
    c->colorspace = AVCOL_SPC_BT709;
    c->color_primaries = AVCOL_PRI_BT709;
    c->color_trc = AVCOL_TRC_BT709;
    c->color_range = AVCOL_RANGE_MPEG;
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (std::strcmp(name, "libx264") == 0) {
        av_opt_set(c->priv_data, "preset", "veryfast", 0);
        av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    } else if (std::strcmp(name, "h264_nvenc") == 0) {
        av_opt_set(c->priv_data, "preset", "p4", 0);
        av_opt_set(c->priv_data, "tune", "ll", 0);
        av_opt_set(c->priv_data, "rc", "cbr", 0);
    }

    const int r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        why = av_err(r);
        avcodec_free_context(&c);
        return nullptr;
    }
    return c;
}

} // namespace

EncoderPtr encoder_open(const EncoderConfig& cfg, std::string& error) {
    auto enc = EncoderPtr(new Encoder());
    enc->cfg = cfg;

    int r = avformat_alloc_output_context2(&enc->fmt, nullptr, nullptr, cfg.path.c_str());
    if (r < 0 || enc->fmt == nullptr) {
        error = std::format("avformat_alloc_output_context2({}): {}", cfg.path, av_err(r));
        return nullptr;
    }

    // Walk the ladder. Pinning a tier is for bisecting a problem, not for normal use.
    struct Rung {
        VideoTier tier;
        const char* name;
    };
    const Rung ladder[] = {
        {VideoTier::Nvenc, "h264_nvenc"},
        {VideoTier::X264, "libx264"},
    };
    for (const Rung& rung : ladder) {
        if (!cfg.force_tier.empty() && cfg.force_tier != rung.name &&
            cfg.force_tier != tier_name(rung.tier))
            continue;
        std::string why;
        enc->vctx = try_video(rung.name, cfg, enc->fmt, why);
        if (enc->vctx != nullptr) {
            enc->stats.tier = rung.tier;
            enc->stats.video_encoder = rung.name;
            break;
        }
        lc::warn("encoder tier {} unavailable: {}", rung.name, why);
    }
    if (enc->vctx == nullptr) {
        error = "no usable video encoder - every tier of the ladder failed";
        return nullptr;
    }
    lc::info("video encoder: {} ({}x{} @ {}/{}, {} kbps)", enc->stats.video_encoder, cfg.width,
             cfg.height, cfg.fps_num, cfg.fps_den, cfg.bitrate / 1000);

    enc->vst = avformat_new_stream(enc->fmt, nullptr);
    if (enc->vst == nullptr) {
        error = "avformat_new_stream (video) failed";
        return nullptr;
    }
    enc->vst->time_base = enc->vctx->time_base;
    avcodec_parameters_from_context(enc->vst->codecpar, enc->vctx);

    enc->vframe = av_frame_alloc();
    enc->vframe->format = enc->vctx->pix_fmt;
    enc->vframe->width = enc->vctx->width;
    enc->vframe->height = enc->vctx->height;
    enc->vframe->colorspace = enc->vctx->colorspace;
    enc->vframe->color_range = enc->vctx->color_range;
    if ((r = av_frame_get_buffer(enc->vframe, 0)) < 0) {
        error = std::format("av_frame_get_buffer (video): {}", av_err(r));
        return nullptr;
    }

    enc->sws = sws_getContext(static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                              AV_PIX_FMT_BGRA, static_cast<int>(cfg.width),
                              static_cast<int>(cfg.height), AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                              nullptr, nullptr, nullptr);
    if (enc->sws == nullptr) {
        error = "sws_getContext failed";
        return nullptr;
    }
    // ADR-0014 again: say which matrix and which range, do not accept a default that varies.
    const int* tbl = sws_getCoefficients(SWS_CS_ITU709);
    sws_setColorspaceDetails(enc->sws, tbl, 1 /* src is full-range RGB */, tbl,
                             0 /* dst is limited-range YUV */, 0, 1 << 16, 1 << 16);

    // --- audio ---
    if (cfg.audio_channels > 0) {
        const AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (acodec == nullptr) {
            error = "no AAC encoder in this libavcodec";
            return nullptr;
        }
        enc->actx = avcodec_alloc_context3(acodec);
        enc->actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        enc->actx->sample_rate = static_cast<int>(cfg.audio_rate);
        enc->actx->bit_rate = cfg.audio_bitrate;
        av_channel_layout_default(&enc->actx->ch_layout, static_cast<int>(cfg.audio_channels));
        enc->actx->time_base = AVRational{1, static_cast<int>(cfg.audio_rate)};
        if ((enc->fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0)
            enc->actx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if ((r = avcodec_open2(enc->actx, acodec, nullptr)) < 0) {
            error = std::format("avcodec_open2 (aac): {}", av_err(r));
            return nullptr;
        }
        enc->stats.audio_encoder = acodec->name;

        enc->ast = avformat_new_stream(enc->fmt, nullptr);
        enc->ast->time_base = enc->actx->time_base;
        avcodec_parameters_from_context(enc->ast->codecpar, enc->actx);

        AVChannelLayout in_layout;
        av_channel_layout_default(&in_layout, static_cast<int>(cfg.audio_channels));
        r = swr_alloc_set_opts2(&enc->swr, &enc->actx->ch_layout, AV_SAMPLE_FMT_FLTP,
                                static_cast<int>(cfg.audio_rate), &in_layout, AV_SAMPLE_FMT_FLT,
                                static_cast<int>(cfg.audio_rate), 0, nullptr);
        if (r < 0 || swr_init(enc->swr) < 0) {
            error = "swr_init failed";
            return nullptr;
        }
        enc->fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, static_cast<int>(cfg.audio_channels),
                                        enc->actx->frame_size * 8);

        enc->aframe = av_frame_alloc();
        enc->aframe->format = AV_SAMPLE_FMT_FLTP;
        enc->aframe->nb_samples = enc->actx->frame_size;
        av_channel_layout_copy(&enc->aframe->ch_layout, &enc->actx->ch_layout);
        if ((r = av_frame_get_buffer(enc->aframe, 0)) < 0) {
            error = std::format("av_frame_get_buffer (audio): {}", av_err(r));
            return nullptr;
        }
        lc::info("audio encoder: {} ({} Hz, {} ch, {} kbps, frame {})", enc->stats.audio_encoder,
                 cfg.audio_rate, cfg.audio_channels, cfg.audio_bitrate / 1000,
                 enc->actx->frame_size);
    }

    if ((enc->fmt->oformat->flags & AVFMT_NOFILE) == 0) {
        if ((r = avio_open(&enc->fmt->pb, cfg.path.c_str(), AVIO_FLAG_WRITE)) < 0) {
            error = std::format("avio_open({}): {}", cfg.path, av_err(r));
            return nullptr;
        }
    }
    // Always write a fragmented container, so an interrupted recording is still a valid file.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    if ((r = avformat_write_header(enc->fmt, &opts)) < 0) {
        av_dict_free(&opts);
        error = std::format("avformat_write_header: {}", av_err(r));
        return nullptr;
    }
    av_dict_free(&opts);
    enc->header_written = true;
    enc->pkt = av_packet_alloc();
    return enc;
}

bool encoder_video(Encoder& enc, const std::uint8_t* bgra, std::size_t stride, std::int64_t pts_ns,
                   std::string& error) {
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    if (av_frame_make_writable(enc.vframe) < 0) {
        error = "av_frame_make_writable (video) failed";
        return false;
    }
    const std::uint8_t* src[4] = {bgra, nullptr, nullptr, nullptr};
    const int src_stride[4] = {static_cast<int>(stride), 0, 0, 0};
    sws_scale(enc.sws, src, src_stride, 0, static_cast<int>(enc.cfg.height), enc.vframe->data,
              enc.vframe->linesize);

    enc.vframe->pts = av_rescale_q(pts_ns - enc.t0_ns, AVRational{1, 1'000'000'000},
                                   enc.vctx->time_base);

    const auto t_begin = std::chrono::steady_clock::now();
    const int r = avcodec_send_frame(enc.vctx, enc.vframe);
    if (r < 0) {
        error = std::format("avcodec_send_frame (video): {}", av_err(r));
        return false;
    }
    if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
        return false;
    enc.stats.video_encode_ms.push_back(
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin)
            .count());
    ++enc.stats.video_frames;
    return true;
}

bool encoder_audio(Encoder& enc, const float* interleaved, std::uint32_t frames,
                   std::int64_t pts_ns, std::string& error) {
    if (enc.actx == nullptr)
        return true;
    if (enc.t0_ns == AV_NOPTS_VALUE)
        enc.t0_ns = pts_ns;

    const std::uint8_t* in[1] = {reinterpret_cast<const std::uint8_t*>(interleaved)};
    // Convert interleaved float into the planar layout AAC wants, straight into the FIFO.
    AVFrame* tmp = av_frame_alloc();
    tmp->format = AV_SAMPLE_FMT_FLTP;
    tmp->nb_samples = static_cast<int>(frames);
    av_channel_layout_copy(&tmp->ch_layout, &enc.actx->ch_layout);
    if (av_frame_get_buffer(tmp, 0) < 0) {
        av_frame_free(&tmp);
        error = "av_frame_get_buffer (audio scratch) failed";
        return false;
    }
    const int converted =
        swr_convert(enc.swr, tmp->data, static_cast<int>(frames), in, static_cast<int>(frames));
    if (converted < 0) {
        av_frame_free(&tmp);
        error = "swr_convert failed";
        return false;
    }
    av_audio_fifo_write(enc.fifo, reinterpret_cast<void**>(tmp->data), converted);
    av_frame_free(&tmp);

    // Drain the FIFO in exact encoder-frame units. AAC will not accept a partial frame, and
    // padding one would inject silence that accumulates as drift.
    while (av_audio_fifo_size(enc.fifo) >= enc.actx->frame_size) {
        if (av_frame_make_writable(enc.aframe) < 0) {
            error = "av_frame_make_writable (audio) failed";
            return false;
        }
        av_audio_fifo_read(enc.fifo, reinterpret_cast<void**>(enc.aframe->data),
                           enc.actx->frame_size);
        // The master clock: an exact running sample count, never a wall-clock reading.
        enc.aframe->pts = enc.audio_samples;
        enc.audio_samples += enc.actx->frame_size;

        const int r = avcodec_send_frame(enc.actx, enc.aframe);
        if (r < 0) {
            error = std::format("avcodec_send_frame (audio): {}", av_err(r));
            return false;
        }
        if (!drain(enc, enc.actx, enc.ast, enc.stats.audio_bytes, error))
            return false;
        ++enc.stats.audio_frames;
    }
    return true;
}

bool encoder_finish(Encoder& enc, std::string& error) {
    if (enc.vctx != nullptr) {
        avcodec_send_frame(enc.vctx, nullptr);
        if (!drain(enc, enc.vctx, enc.vst, enc.stats.video_bytes, error))
            return false;
    }
    if (enc.actx != nullptr) {
        avcodec_send_frame(enc.actx, nullptr);
        if (!drain(enc, enc.actx, enc.ast, enc.stats.audio_bytes, error))
            return false;
    }
    const int r = av_write_trailer(enc.fmt);
    if (r < 0) {
        error = std::format("av_write_trailer: {}", av_err(r));
        return false;
    }
    enc.header_written = false;
    return true;
}

const EncoderStats& encoder_stats(Encoder& enc) { return enc.stats; }

void encoder_close(Encoder* enc) {
    if (enc == nullptr)
        return;
    if (enc->header_written)
        av_write_trailer(enc->fmt);
    if (enc->pkt != nullptr)
        av_packet_free(&enc->pkt);
    if (enc->sws != nullptr)
        sws_freeContext(enc->sws);
    if (enc->swr != nullptr)
        swr_free(&enc->swr);
    if (enc->fifo != nullptr)
        av_audio_fifo_free(enc->fifo);
    if (enc->vframe != nullptr)
        av_frame_free(&enc->vframe);
    if (enc->aframe != nullptr)
        av_frame_free(&enc->aframe);
    if (enc->vctx != nullptr)
        avcodec_free_context(&enc->vctx);
    if (enc->actx != nullptr)
        avcodec_free_context(&enc->actx);
    if (enc->fmt != nullptr) {
        if ((enc->fmt->oformat->flags & AVFMT_NOFILE) == 0 && enc->fmt->pb != nullptr)
            avio_closep(&enc->fmt->pb);
        avformat_free_context(enc->fmt);
    }
    delete enc;
}

} // namespace libertas::spike
