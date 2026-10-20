// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "lcapture/video.hpp"
#include <lcore/log.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace libertas::capture {
namespace {
int call(int fd, unsigned long request, void* arg) {
    int result;
    do { result = ioctl(fd, request, arg); } while (result < 0 && errno == EINTR);
    return result;
}
bool integer(std::string_view text, int& value) {
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() && value > 0;
}
bool capture_node(int fd, v4l2_capability& caps) {
    if (call(fd, VIDIOC_QUERYCAP, &caps) < 0) return false;
    auto flags = (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
    return (flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING);
}
AVPixelFormat pixel_format(std::uint32_t fmt) {
    switch (fmt) {
    case V4L2_PIX_FMT_NV12: return AV_PIX_FMT_NV12;
    case V4L2_PIX_FMT_YUYV: return AV_PIX_FMT_YUYV422;
    case V4L2_PIX_FMT_YUV420: return AV_PIX_FMT_YUV420P;
    case V4L2_PIX_FMT_YVU420: return AV_PIX_FMT_YUV420P;
    case V4L2_PIX_FMT_P010: return AV_PIX_FMT_P010LE;
    case V4L2_PIX_FMT_BGR24: return AV_PIX_FMT_BGR24;
    case V4L2_PIX_FMT_RGB24: return AV_PIX_FMT_RGB24;
    default: return AV_PIX_FMT_NONE;
    }
}
std::string fourcc(std::uint32_t code) {
    std::string text(4, ' ');
    for (unsigned i = 0; i < 4; ++i) text[i] = static_cast<char>((code >> (8 * i)) & 0xff);
    return text;
}
bool supported(std::uint32_t code) {
    return pixel_format(code) != AV_PIX_FMT_NONE ||
           (code == V4L2_PIX_FMT_MJPEG && avcodec_find_decoder(AV_CODEC_ID_MJPEG));
}
}

std::vector<VideoFormat> list_video_formats(const std::string& path) {
    std::vector<VideoFormat> formats;
    if (!path.starts_with("/dev/video") && !path.starts_with("/dev/v4l/")) return formats;
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return formats;
    v4l2_fmtdesc description{};
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (call(fd, VIDIOC_ENUM_FMT, &description) == 0) {
        if (supported(description.pixelformat)) {
            std::string name(reinterpret_cast<const char*>(description.description),
                             strnlen(reinterpret_cast<const char*>(description.description),
                                     sizeof(description.description)));
            if (description.flags & V4L2_FMT_FLAG_EMULATED) name += " (Emulated)";
            formats.push_back({fourcc(description.pixelformat), std::move(name)});
        }
        ++description.index;
    }
    ::close(fd);
    return formats;
}

std::vector<VideoDevice> list_video_devices() {
    std::vector<VideoDevice> result;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/video4linux", ec)) {
        const auto node = "/dev/" + entry.path().filename().string();
        const int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability caps{};
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (capture_node(fd, caps) && call(fd, VIDIOC_G_FMT, &fmt) == 0) {
            std::string path = node;
            for (const auto& link : std::filesystem::directory_iterator("/dev/v4l/by-id", ec)) {
                if (std::filesystem::canonical(link.path(), ec) == node) {
                    path = link.path().string();
                    break;
                }
            }
            result.push_back({path, reinterpret_cast<const char*>(caps.card),
                static_cast<int>(fmt.fmt.pix.width), static_cast<int>(fmt.fmt.pix.height)});
        }
        ::close(fd);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}

struct VideoCapture::State {
    int fd = -1;
    struct Buffer { void* data = MAP_FAILED; std::size_t size = 0; };
    std::vector<Buffer> buffers;
    v4l2_format format{};
    SwsContext* converter = nullptr;
    AVCodecContext* decoder = nullptr;
    AVFrame* decoded = nullptr;
    AVPacket* packet = nullptr;
    VideoOptions options;
    std::string colour_range;
    std::chrono::nanoseconds timeout{};
    std::atomic<bool> stopping{false};
    std::thread thread;
    bool streaming = false;
    ~State() {
        stopping = true;
        if (thread.joinable()) thread.join();
        if (streaming) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)call(fd, VIDIOC_STREAMOFF, &type);
        }
        for (auto& buffer : buffers)
            if (buffer.data != MAP_FAILED) ::munmap(buffer.data, buffer.size);
        sws_freeContext(converter);
        av_packet_free(&packet);
        av_frame_free(&decoded);
        avcodec_free_context(&decoder);
        if (fd >= 0) ::close(fd);
    }
    bool reset() {
        auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (call(fd, VIDIOC_STREAMOFF, &type) < 0) return false;
        streaming = false;
        for (std::size_t i = 0; i < buffers.size(); ++i) {
            v4l2_buffer buffer{};
            buffer.type = type;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index = static_cast<unsigned>(i);
            if (call(fd, VIDIOC_QBUF, &buffer) < 0) return false;
        }
        if (call(fd, VIDIOC_STREAMON, &type) < 0) return false;
        streaming = true;
        return true;
    }
    void run(const Callback& callback) {
        const auto& f = format.fmt.pix;
        std::array<std::shared_ptr<VideoFrame>, 3> pool;
        for (auto& frame : pool) {
            frame = std::make_shared<VideoFrame>();
            frame->width = static_cast<int>(f.width);
            frame->height = static_cast<int>(f.height);
            frame->pixels.resize(static_cast<std::size_t>(f.width) * f.height * 4);
        }
        auto deadline = std::chrono::steady_clock::now() + timeout;
        bool reported_timeout = false;
        while (!stopping) {
            pollfd event{fd, POLLIN, 0};
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            const int ready = ::poll(&event, 1, static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 100)));
            if (ready < 0 && errno == EINTR) continue;
            if (ready == 0) {
                if (std::chrono::steady_clock::now() < deadline) continue;
                if (!reported_timeout) lc::warn("video capture timed out after {} frame periods", options.timeout_frames);
                reported_timeout = true;
                if (options.auto_reset && !reset()) {
                    lc::warn("cannot reset video capture: {}", std::strerror(errno));
                    break;
                }
                deadline = std::chrono::steady_clock::now() + timeout;
                continue;
            }
            if (ready < 0 || (event.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
            v4l2_buffer buffer{};
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (call(fd, VIDIOC_DQBUF, &buffer) < 0) {
                if (errno == EAGAIN) continue;
                break;
            }
            if (buffer.index >= buffers.size()) break;
            // Keep one buffer dequeued. Bound draining so a fast producer cannot starve conversion.
            if (!options.buffering) {
                bool failed = false;
                for (std::size_t i = 1; i < buffers.size(); ++i) {
                    auto next = buffer;
                    if (call(fd, VIDIOC_DQBUF, &next) < 0) {
                        failed = errno != EAGAIN;
                        break;
                    }
                    if (next.index >= buffers.size() || call(fd, VIDIOC_QBUF, &buffer) < 0) {
                        failed = true;
                        break;
                    }
                    buffer = next;
                }
                if (failed) break;
            }
            deadline = std::chrono::steady_clock::now() + timeout;
            reported_timeout = false;
            auto free = std::find_if(pool.begin(), pool.end(), [](const auto& p) { return p.use_count() == 1; });
            const std::size_t plane = static_cast<std::size_t>(f.bytesperline) * f.height;
            std::size_t need = plane;
            if (f.pixelformat == V4L2_PIX_FMT_NV12 || f.pixelformat == V4L2_PIX_FMT_YUV420 ||
                f.pixelformat == V4L2_PIX_FMT_YVU420 || f.pixelformat == V4L2_PIX_FMT_P010)
                need += plane / 2;
            if (decoder) need = 1;
            if (free != pool.end() && !(buffer.flags & V4L2_BUF_FLAG_ERROR) &&
                buffer.bytesused >= need && buffer.bytesused <= buffers[buffer.index].size) {
                const auto* data = static_cast<const std::uint8_t*>(buffers[buffer.index].data);
                const std::uint8_t* src[4] = {data, nullptr, nullptr, nullptr};
                int stride[4] = {static_cast<int>(f.bytesperline), 0, 0, 0};
                bool valid = true;
                if (decoder) {
                    av_packet_unref(packet);
                    av_frame_unref(decoded);
                    valid = av_new_packet(packet, static_cast<int>(buffer.bytesused)) >= 0;
                    if (valid) {
                        std::memcpy(packet->data, data, buffer.bytesused);
                        valid = avcodec_send_packet(decoder, packet) >= 0 &&
                                avcodec_receive_frame(decoder, decoded) >= 0 &&
                                decoded->width == static_cast<int>(f.width) &&
                                decoded->height == static_cast<int>(f.height);
                    }
                    if (valid) {
                        converter = sws_getCachedContext(converter, decoded->width, decoded->height,
                            static_cast<AVPixelFormat>(decoded->format), decoded->width, decoded->height,
                            AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                        valid = converter != nullptr;
                        if (valid) {
                            const auto* matrix = sws_getCoefficients(decoded->colorspace == AVCOL_SPC_BT709
                                ? SWS_CS_ITU709 : SWS_CS_ITU601);
                            const int full = colour_range == "Full" ||
                                (colour_range != "Partial" && decoded->color_range == AVCOL_RANGE_JPEG);
                            valid = sws_setColorspaceDetails(converter, matrix, full, matrix, 1,
                                                            0, 1 << 16, 1 << 16) >= 0;
                        }
                        for (int i = 0; i < 4; ++i) {
                            src[i] = decoded->data[i];
                            stride[i] = decoded->linesize[i];
                        }
                    }
                } else if (f.pixelformat == V4L2_PIX_FMT_NV12 || f.pixelformat == V4L2_PIX_FMT_P010) {
                    src[1] = data + plane;
                    stride[1] = stride[0];
                } else if (f.pixelformat == V4L2_PIX_FMT_YUV420 || f.pixelformat == V4L2_PIX_FMT_YVU420) {
                    src[1] = data + plane;
                    src[2] = src[1] + plane / 4;
                    stride[1] = stride[2] = stride[0] / 2;
                    if (f.pixelformat == V4L2_PIX_FMT_YVU420) std::swap(src[1], src[2]);
                }
                std::uint8_t* dst[4] = {(*free)->pixels.data(), nullptr, nullptr, nullptr};
                int dst_stride[4] = {static_cast<int>(f.width) * 4, 0, 0, 0};
                if (valid && sws_scale(converter, src, stride, 0, static_cast<int>(f.height), dst, dst_stride)
                    == static_cast<int>(f.height)) callback(*free);
            }
            if (call(fd, VIDIOC_QBUF, &buffer) < 0) break;
        }
        if (!stopping) lc::warn("video capture stopped: device disconnected or streaming failed");
        callback({});
    }
};

VideoCapture::VideoCapture(std::unique_ptr<State> state) : state_(std::move(state)) {}
VideoCapture::~VideoCapture() = default;
void VideoCapture::stop() {
    state_->stopping = true;
    if (state_->thread.joinable()) state_->thread.join();
}
void VideoCapture::start(Callback callback) {
    if (!state_->thread.joinable())
        state_->thread = std::thread([this, callback = std::move(callback)] { state_->run(callback); });
}

std::unique_ptr<VideoCapture> VideoCapture::open(const std::string& path,
    const std::string& resolution, const std::string& rate, const std::string& range,
    std::string& error, const VideoOptions& options) {
    auto state = std::make_unique<State>();
    auto fail = [&](const std::string& why) -> std::unique_ptr<VideoCapture> { error = why; return {}; };
    if (options.timeout_frames < 2 || options.timeout_frames > 120)
        return fail("frames until timeout must be an integer from 2 to 120");
    state->options = options;
    state->colour_range = range;
    if (!path.starts_with("/dev/video") && !path.starts_with("/dev/v4l/"))
        return fail("select a V4L2 video device");
    state->fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (state->fd < 0) return fail(std::strerror(errno));
    v4l2_capability caps{};
    if (!capture_node(state->fd, caps)) return fail("device is not a streaming video capture node");
    auto& fmt = state->format;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (call(state->fd, VIDIOC_G_FMT, &fmt) < 0) return fail(std::strerror(errno));
    v4l2_streamparm param{};
    param.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (call(state->fd, VIDIOC_G_PARM, &param) < 0) return fail(std::strerror(errno));
    int width = static_cast<int>(fmt.fmt.pix.width), height = static_cast<int>(fmt.fmt.pix.height);
    if (!resolution.empty() && resolution != "Leave unchanged") {
        const auto x = resolution.find('x');
        if (x == std::string::npos || !integer(std::string_view(resolution).substr(0, x), width) ||
            !integer(std::string_view(resolution).substr(x + 1), height)) return fail("invalid capture resolution");
    }
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192 || width % 2 || height % 2)
        return fail("unsupported capture dimensions");
    // Planar 4:2:0 keeps NV12's USB bandwidth but uses swscale's fast unscaled RGB converter.
    // Measured at 1440p: YUV420 1.19 ms, NV12 7.80 ms. Retain NV12 for devices without YU12.
    bool found = false;
    std::uint32_t selected = 0;
    if (!options.format.empty() && options.format != "Auto") {
        if (options.format.size() != 4) return fail("invalid video format");
        for (unsigned i = 0; i < 4; ++i)
            selected |= static_cast<std::uint32_t>(static_cast<unsigned char>(options.format[i])) << (8 * i);
        if (!supported(selected)) return fail("unsupported video format");
    }
    for (auto code : {V4L2_PIX_FMT_YUV420, V4L2_PIX_FMT_NV12, V4L2_PIX_FMT_BGR24,
                      V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB24, V4L2_PIX_FMT_MJPEG,
                      V4L2_PIX_FMT_YVU420, V4L2_PIX_FMT_P010}) {
        if (selected && code != selected) continue;
        fmt.fmt.pix.width = static_cast<std::uint32_t>(width);
        fmt.fmt.pix.height = static_cast<std::uint32_t>(height);
        fmt.fmt.pix.pixelformat = code;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (call(state->fd, VIDIOC_TRY_FMT, &fmt) == 0 && fmt.fmt.pix.pixelformat == code &&
            fmt.fmt.pix.width == static_cast<unsigned>(width) && fmt.fmt.pix.height == static_cast<unsigned>(height)) {
            found = true;
            break;
        }
    }
    if (!found) return fail("video format is not supported at this resolution");
    if (call(state->fd, VIDIOC_S_FMT, &fmt) < 0) return fail(std::strerror(errno));
    if (fmt.fmt.pix.width != static_cast<unsigned>(width) || fmt.fmt.pix.height != static_cast<unsigned>(height) ||
        !supported(fmt.fmt.pix.pixelformat) || (selected && fmt.fmt.pix.pixelformat != selected))
        return fail("device changed requested capture mode");
    const auto& f = fmt.fmt.pix;
    const bool compressed = f.pixelformat == V4L2_PIX_FMT_MJPEG;
    if (f.field != V4L2_FIELD_NONE) return fail("interlaced capture is not supported yet");
    const unsigned bpp = (f.pixelformat == V4L2_PIX_FMT_BGR24 || f.pixelformat == V4L2_PIX_FMT_RGB24) ? 3u :
                         (f.pixelformat == V4L2_PIX_FMT_YUYV || f.pixelformat == V4L2_PIX_FMT_P010 ? 2u : 1u);
    if ((!compressed && (f.bytesperline < f.width * bpp || f.bytesperline > 65536)) ||
        !f.sizeimage || f.sizeimage > 512u * 1024u * 1024u) return fail("invalid capture buffer layout");
    const bool planar = f.pixelformat == V4L2_PIX_FMT_NV12 || f.pixelformat == V4L2_PIX_FMT_YUV420 ||
                        f.pixelformat == V4L2_PIX_FMT_YVU420 || f.pixelformat == V4L2_PIX_FMT_P010;
    const std::size_t plane = static_cast<std::size_t>(f.bytesperline) * f.height;
    if (!compressed && ((planar && f.bytesperline % 2) || f.sizeimage < plane + (planar ? plane / 2 : 0)))
        return fail("invalid capture plane layout");
    if (!rate.empty() && rate != "Leave unchanged") {
        int num = 0, den = 1;
        if (rate == "59.94") { num = 60000; den = 1001; }
        else if (rate == "29.97") { num = 30000; den = 1001; }
        else if (!integer(rate, num) || num > 240) return fail("invalid capture frame rate");
        param.parm.capture.timeperframe = {static_cast<unsigned>(den), static_cast<unsigned>(num)};
    }
    const auto requested = param.parm.capture.timeperframe;
    if (!requested.numerator || !requested.denominator) return fail("invalid capture frame rate");
    if (call(state->fd, VIDIOC_S_PARM, &param) < 0) return fail(std::strerror(errno));
    const auto interval = param.parm.capture.timeperframe;
    if (!interval.numerator || !interval.denominator ||
        static_cast<std::uint64_t>(interval.numerator) * requested.denominator !=
        static_cast<std::uint64_t>(interval.denominator) * requested.numerator)
        return fail("device does not support requested frame rate");
    const double timeout_seconds = static_cast<double>(options.timeout_frames) * interval.numerator / interval.denominator;
    if (timeout_seconds <= 0 || timeout_seconds > 3600) return fail("invalid capture timeout interval");
    state->timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(timeout_seconds));
    if (f.xfer_func == V4L2_XFER_FUNC_SMPTE2084 || f.colorspace == V4L2_COLORSPACE_BT2020)
        return fail("HDR or BT.2020 capture is not supported yet");
    if (compressed) {
        const auto* codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
        state->decoder = avcodec_alloc_context3(codec);
        state->decoded = av_frame_alloc();
        state->packet = av_packet_alloc();
        if (state->decoder) state->decoder->max_pixels = static_cast<std::int64_t>(width) * height;
        if (!codec || !state->decoder || !state->decoded || !state->packet ||
            avcodec_open2(state->decoder, codec, nullptr) < 0)
            return fail("cannot open Motion-JPEG decoder");
    } else {
        state->converter = sws_getContext(width, height, pixel_format(f.pixelformat), width, height,
                                         AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!state->converter) return fail("cannot create capture pixel converter");
        int matrix = f.ycbcr_enc == V4L2_YCBCR_ENC_709 || f.colorspace == V4L2_COLORSPACE_REC709
                         ? SWS_CS_ITU709 : SWS_CS_ITU601;
        int full = f.quantization == V4L2_QUANTIZATION_FULL_RANGE || f.colorspace == V4L2_COLORSPACE_JPEG;
        if (range == "Full") full = 1;
        else if (range == "Partial") full = 0;
        const int* coefficients = sws_getCoefficients(matrix);
        if (sws_setColorspaceDetails(state->converter, coefficients, full, coefficients, 1, 0, 1 << 16, 1 << 16) < 0)
            return fail("unsupported capture colour conversion");
    }
    if (!range.empty() && range != "Default" && range != "Full" && range != "Partial")
        return fail("invalid colour range");
    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (call(state->fd, VIDIOC_REQBUFS, &request) < 0 || request.count < 2 || request.count > 32)
        return fail("cannot allocate capture buffers (device may be busy)");
    for (unsigned i = 0; i < request.count; ++i) {
        v4l2_buffer buffer{};
        buffer.type = request.type;
        buffer.memory = request.memory;
        buffer.index = i;
        if (call(state->fd, VIDIOC_QUERYBUF, &buffer) < 0 || buffer.length < f.sizeimage ||
            buffer.length > 512u * 1024u * 1024u) return fail("invalid mapped capture buffer");
        void* data = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, state->fd, buffer.m.offset);
        if (data == MAP_FAILED) return fail(std::strerror(errno));
        state->buffers.push_back({data, buffer.length});
        if (call(state->fd, VIDIOC_QBUF, &buffer) < 0) return fail(std::strerror(errno));
    }
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (call(state->fd, VIDIOC_STREAMON, &type) < 0) return fail(std::strerror(errno));
    state->streaming = true;
    lc::info("video capture: {}x{} {} at {}/{} fps on {}", width, height, fourcc(f.pixelformat),
        param.parm.capture.timeperframe.denominator, param.parm.capture.timeperframe.numerator, path);
    return std::unique_ptr<VideoCapture>(new VideoCapture(std::move(state)));
}
} // namespace libertas::capture
