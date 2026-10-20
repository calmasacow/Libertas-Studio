// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace libertas::capture {
struct VideoDevice {
    std::string path, name;
    int width = 0, height = 0;
};
std::vector<VideoDevice> list_video_devices();
struct VideoFormat { std::string code, name; };
std::vector<VideoFormat> list_video_formats(const std::string& path);
struct VideoOptions {
    std::string format; // Empty or Auto selects a supported format.
    bool buffering = true;
    bool auto_reset = false;
    int timeout_frames = 5;
    friend bool operator==(const VideoOptions&, const VideoOptions&) = default;
};
struct VideoFrame {
    int width = 0, height = 0;
    std::vector<std::uint8_t> pixels; // Tightly packed BGRA8.
};
class VideoCapture {
  public:
    using Callback = std::function<void(std::shared_ptr<const VideoFrame>)>;
    static std::unique_ptr<VideoCapture> open(const std::string& path,
        const std::string& resolution, const std::string& rate, const std::string& range,
        std::string& error, const VideoOptions& options = {});
    ~VideoCapture();
    void start(Callback callback);
    void stop();
  private:
    struct State;
    explicit VideoCapture(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace libertas::capture
