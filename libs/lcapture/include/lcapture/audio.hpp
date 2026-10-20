// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace libertas::capture {
struct AudioDevice {
    std::string id, name;
    bool monitor = false;
};

std::vector<AudioDevice> list_audio_devices(std::string& error);
std::vector<AudioDevice> list_audio_sinks(std::string& error);

class AudioCapture {
  public:
    using Callback = std::function<void(std::span<const float>)>;
    static std::unique_ptr<AudioCapture> open(const std::string& device, bool output,
                                              Callback callback, std::string& error);
    ~AudioCapture();

  private:
    struct State;
    explicit AudioCapture(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

class AudioMonitor {
  public:
    static std::unique_ptr<AudioMonitor> open(std::string& error, const std::string& device = {});
    ~AudioMonitor();
    void write(std::span<const float> samples);

  private:
    struct State;
    explicit AudioMonitor(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

class AudioSink {
  public:
    static std::unique_ptr<AudioSink> open(const std::string& name, std::string& error);
    ~AudioSink();

  private:
    struct State;
    explicit AudioSink(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};
} // namespace libertas::capture
