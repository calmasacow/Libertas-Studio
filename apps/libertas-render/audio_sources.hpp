// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <lcapture/audio.hpp>
#include <lcomp/preview.hpp>
#include <lcore/log.hpp>
#include <map>
#include <mutex>
#include <set>
#include <thread>

class AudioSources {
    struct Session {
        ldoc::Source source;
        std::mutex mu;
        float left = 0, right = 0;
        bool available = false;
        std::deque<float> pcm;
        std::chrono::steady_clock::time_point received;

        void ingest(std::span<const float> samples) {
            std::lock_guard lock(mu);
            available = !samples.empty();
            left = right = 0;
            for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
                if (std::isfinite(samples[i]))
                    left = std::max(left, std::abs(samples[i]));
                if (std::isfinite(samples[i + 1]))
                    right = std::max(right, std::abs(samples[i + 1]));
            }
            for (float sample : samples)
                pcm.push_back(std::isfinite(sample) ? sample : 0);
            while (pcm.size() > 9600)
                pcm.pop_front();
            received = std::chrono::steady_clock::now();
        }

        std::jthread thread;

        explicit Session(const ldoc::Source& src)
            : source(src), thread([this](std::stop_token stop) {
                  if (source.type != "source.audio-input" && source.type != "source.audio-output" &&
                      source.type != "source.v4l2")
                      return;
                  const bool video = source.type == "source.v4l2";
                  std::string device = "default", error;
                  if (auto it = source.settings.find(video ? "audioDevice" : "device");
                      it != source.settings.end())
                      if (auto value = std::get_if<std::string>(&it->second))
                          device = *value;
                  while (!stop.stop_requested()) {
                      error.clear();
                      std::string selected = device;
                      if (video && device == "default") {
                          const auto it = source.settings.find("device");
                          const auto* path = it == source.settings.end()
                                                 ? nullptr
                                                 : std::get_if<std::string>(&it->second);
                          selected =
                              libertas::capture::video_audio_device(path ? *path : "", error);
                      }
                      std::unique_ptr<libertas::capture::AudioCapture> capture;
                      if (!video || !selected.empty())
                          capture = libertas::capture::AudioCapture::open(
                              selected, source.type == "source.audio-output",
                              [this](std::span<const float> samples) { ingest(samples); }, error,
                              video);
                      if (!capture) {
                          lc::warn("audio {}: {}", source.name, error);
                          if (!video)
                              return;
                          for (int i = 0; i < 150 && !stop.stop_requested(); ++i)
                              std::this_thread::sleep_for(std::chrono::milliseconds(20));
                          continue;
                      }
                      const auto opened = std::chrono::steady_clock::now();
                      while (!stop.stop_requested()) {
                          std::this_thread::sleep_for(std::chrono::milliseconds(20));
                          if (video) {
                              std::lock_guard lock(mu);
                              if (std::chrono::steady_clock::now() - std::max(opened, received) >
                                  std::chrono::seconds(3))
                                  break;
                          }
                      }
                  }
              }) {}

        ~Session() {
            thread.request_stop();
            if (thread.joinable())
                thread.join();
        }
    };

    std::map<ldoc::Id, std::shared_ptr<Session>> sessions_;

    struct Route {
        ldoc::Id node;
        std::shared_ptr<Session> session;
        ldoc::AudioRouting audio;
        bool unsupported_effect = false;
    };

    std::mutex mu_;
    std::vector<Route> routes_;
    std::vector<ldoc::Bus> buses_;
    std::string monitor_output_ = "none";
    std::map<ldoc::Id, lcomp::AudioLevel> bus_levels_;
    std::jthread mixer_{[this](std::stop_token stop) { mix(stop); }};

    static float gain(double db) {
        return std::isfinite(db)
                   ? static_cast<float>(std::pow(10.0, std::clamp(db, -120.0, 24.0) / 20))
                   : 0;
    }

    void mix(std::stop_token stop) {
        using Block = std::array<float, 960>;
        std::unique_ptr<libertas::capture::AudioMonitor> monitor;
        std::string opened_output;
        auto retry_monitor = std::chrono::steady_clock::time_point{};

        struct Delay {
            std::size_t length = 0;
            std::deque<float> samples;
        };

        std::map<ldoc::Id, Delay> delays;
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested()) {
            next += std::chrono::milliseconds(10);
            std::vector<Route> routes;
            std::vector<ldoc::Bus> buses;
            std::string monitor_output;
            {
                std::lock_guard lock(mu_);
                routes = routes_;
                buses = buses_;
                monitor_output = monitor_output_;
            }
            std::map<Session*, Block> inputs;
            std::map<ldoc::Id, Block> mixed;
            Block listening{};
            bool monitoring = false;
            auto add = [](Block& dst, const Block& src, float volume) {
                for (std::size_t i = 0; i < dst.size(); ++i)
                    dst[i] += src[i] * volume;
            };
            for (const auto& route : routes) {
                if (route.unsupported_effect)
                    continue;
                auto* session = route.session.get();
                if (!inputs.contains(session)) {
                    auto& block = inputs[session];
                    std::lock_guard lock(session->mu);
                    for (auto& value : block) {
                        if (session->pcm.empty())
                            break;
                        value = session->pcm.front();
                        session->pcm.pop_front();
                    }
                }
                const auto& input = inputs.at(session);
                const float volume = route.audio.mute ? 0 : gain(route.audio.gain_db);
                if (route.audio.bus)
                    add(mixed[*route.audio.bus], input, volume);
                for (const auto& send : route.audio.sends)
                    add(mixed[send.to], input,
                        (send.pre_fader ? (route.audio.mute ? 0 : 1) : volume) *
                            gain(send.gain_db));
                if (route.audio.monitor) {
                    monitoring = true;
                    add(listening, input, volume);
                }
            }
            std::set<ldoc::Id> finished;
            std::map<ldoc::Id, lcomp::AudioLevel> levels;
            for (std::size_t pass = 0; pass < buses.size(); ++pass) {
                bool progress = false;
                for (const auto& bus : buses) {
                    if (finished.contains(bus.id))
                        continue;
                    bool waiting = false;
                    for (const auto& upstream : buses)
                        if (!finished.contains(upstream.id))
                            for (const auto& send : upstream.sends)
                                waiting = waiting || send.to == bus.id;
                    if (waiting)
                        continue;
                    const auto input = mixed[bus.id];
                    auto& output = mixed[bus.id];
                    const bool unsupported = std::ranges::any_of(
                        bus.inserts, [](const auto& effect) { return effect.enabled; });
                    const float volume = bus.mute || unsupported ? 0 : gain(bus.gain_db);
                    const double pan = std::isfinite(bus.pan) ? std::clamp(bus.pan, -1.0, 1.0) : 0;
                    lcomp::AudioLevel level{bus.id, 0, 0, true};
                    for (std::size_t i = 0; i < output.size(); i += 2) {
                        output[i] *= volume * static_cast<float>(std::min(1.0, 1.0 - pan));
                        output[i + 1] *= volume * static_cast<float>(std::min(1.0, 1.0 + pan));
                        if (bus.channels == 1)
                            output[i] = output[i + 1] = (output[i] + output[i + 1]) * 0.5f;
                        level.left = std::max(level.left, std::abs(output[i]));
                        level.right = std::max(level.right, std::abs(output[i + 1]));
                    }
                    auto& delay = delays[bus.id];
                    const auto length = std::isfinite(bus.delay_ms)
                                            ? static_cast<std::size_t>(
                                                  std::clamp(bus.delay_ms, 0.0, 10000.0) * 48) *
                                                  2
                                            : 0;
                    if (delay.length != length) {
                        delay.length = length;
                        delay.samples.assign(length, 0);
                    }
                    if (length)
                        for (auto& sample : output) {
                            delay.samples.push_back(sample);
                            sample = delay.samples.front();
                            delay.samples.pop_front();
                        }
                    level.available = !unsupported;
                    levels[bus.id] = level;
                    for (const auto& send : bus.sends)
                        add(mixed[send.to], send.pre_fader ? input : output,
                            bus.mute ? 0 : gain(send.gain_db));
                    if (bus.monitor) {
                        monitoring = true;
                        add(listening, output, 1);
                    }
                    finished.insert(bus.id);
                    progress = true;
                }
                if (!progress)
                    break;
            }
            std::erase_if(delays, [&](const auto& item) {
                return std::ranges::none_of(buses,
                                            [&](const auto& bus) { return bus.id == item.first; });
            });
            {
                std::lock_guard lock(mu_);
                bus_levels_ = std::move(levels);
            }
            if (monitor_output != opened_output) {
                monitor.reset();
                opened_output = monitor_output;
                retry_monitor = {};
            }
            if (!monitoring || monitor_output == "none") {
                monitor.reset();
            } else if (!monitor && std::chrono::steady_clock::now() >= retry_monitor) {
                std::string error;
                monitor = libertas::capture::AudioMonitor::open(
                    error, monitor_output == "default" ? "" : monitor_output.substr(5));
                if (!monitor) {
                    retry_monitor = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    lc::warn("audio monitor: {}", error);
                } else {
                    retry_monitor = {};
                }
            }
            if (monitor)
                monitor->write(listening);
            if (next < std::chrono::steady_clock::now())
                next = std::chrono::steady_clock::now();
            std::this_thread::sleep_until(next);
        }
    }

  public:
    ~AudioSources() {
        mixer_.request_stop();
        if (mixer_.joinable())
            mixer_.join();
    }

    void ingest(const ldoc::Id& source, std::span<const float> samples) {
        std::lock_guard lock(mu_);
        for (const auto& route : routes_)
            if (route.session->source.id == source) {
                route.session->ingest(samples);
                break;
            }
    }

    std::vector<lcomp::AudioLevel> meters(const ldoc::Project& project) {
        std::set<ldoc::Id> wanted, visited;
        std::vector<lcomp::AudioLevel> result;
        std::vector<Route> routes;
        const ldoc::Scene* owner = nullptr;
        bool inherited_effect = false;
        const auto active_effect = [](const auto& effects) {
            return std::ranges::any_of(effects, [](const auto& effect) { return effect.enabled; });
        };
        const auto session_for = [&](const ldoc::Source& source) {
            wanted.insert(source.id);
            auto& session = sessions_[source.id];
            const auto setting = [](const ldoc::Source& src, const char* key) {
                const auto it = src.settings.find(key);
                return it == src.settings.end() ? ldoc::ParamValue{} : it->second;
            };
            if (session &&
                (session->source.type != source.type ||
                 (source.type == "source.v4l2"
                      ? setting(session->source, "audioDevice") != setting(source, "audioDevice") ||
                            setting(session->source, "device") != setting(source, "device")
                      : session->source.settings != source.settings)))
                session.reset();
            if (!session)
                session = std::make_shared<Session>(source);
            return session;
        };
        for (const auto& source : project.sources) {
            if (source.type != "source.v4l2" || !ldoc::has_audio(source))
                continue;
            const auto it = source.settings.find("deactivateWhenHidden");
            const auto* deactivate =
                it == source.settings.end() ? nullptr : std::get_if<bool>(&it->second);
            if (!deactivate || !*deactivate)
                session_for(source);
        }
        std::function<void(const std::vector<ldoc::Node>&)> walk;
        walk = [&](const std::vector<ldoc::Node>& nodes) {
            for (const auto& node : nodes) {
                if (!node.visible || !visited.insert(node.id).second)
                    continue;
                const bool previous_effect = inherited_effect;
                inherited_effect = inherited_effect || active_effect(node.effects);
                if (const auto* group = std::get_if<ldoc::GroupData>(&node.data))
                    walk(group->children);
                else if (const auto* pre = std::get_if<ldoc::PrecompData>(&node.data)) {
                    if (const auto* scene = project.scene(pre->scene)) {
                        const auto* previous = owner; owner = scene;
                        inherited_effect = inherited_effect || active_effect(scene->effects);
                        walk(scene->roots);
                        owner = previous;
                    }
                } else if (const auto* data = std::get_if<ldoc::SourceData>(&node.data)) {
                    const auto* source = data->source ? project.source(*data->source) : nullptr;
                    if (!source || !ldoc::has_audio(*source) ||
                        (source->type != "source.audio-input" &&
                         source->type != "source.audio-output" && source->type != "source.media" &&
                         source->type != "source.playlist" && source->type != "source.blackmagic" &&
                         source->type != "source.browser" && source->type != "source.v4l2")) {
                        inherited_effect = previous_effect;
                        continue;
                    }
                    auto session = session_for(*source);
                    auto routing = data->audio;
                    if (owner) if (auto found = owner->audio_overrides.find(node.id); found != owner->audio_overrides.end()) {
                        const auto& change = found->second;
                        if (change.bus) routing.bus = change.bus;
                        if (change.gain_db) routing.gain_db = *change.gain_db;
                        if (change.mute) routing.mute = *change.mute;
                        if (change.sends) routing.sends = *change.sends;
                    }
                    const bool unsupported = inherited_effect || active_effect(source->effects);
                    routes.push_back({node.id, session, std::move(routing), unsupported});
                    std::lock_guard lock(session->mu);
                    const bool available = !unsupported && session->available &&
                                           std::chrono::steady_clock::now() - session->received <
                                               std::chrono::seconds(1);
                    result.push_back({node.id, available ? session->left : 0,
                                      available ? session->right : 0, available});
                }
                inherited_effect = previous_effect;
            }
        };
        for (const auto& deck : project.decks)
            if (deck.program_scene)
                if (const auto* scene = project.scene(*deck.program_scene)) {
                    owner = scene;
                    inherited_effect = active_effect(scene->effects);
                    walk(scene->roots);
                }
        std::erase_if(sessions_, [&](const auto& item) { return !wanted.contains(item.first); });
        {
            std::lock_guard lock(mu_);
            routes_ = std::move(routes);
            buses_ = project.buses;
            monitor_output_ = "none";
            if (const auto it = project.settings.extra.find("audio.monitor.output");
                it != project.settings.extra.end()) {
                if (const auto* value = std::get_if<std::string>(&it->second);
                    value && !value->empty() && value->size() <= 1024 &&
                    value->find('\0') == std::string::npos &&
                    (*value == "default" || (value->starts_with("sink:") && value->size() > 5)))
                    monitor_output_ = *value;
            }
            for (const auto& [id, level] : bus_levels_)
                result.push_back(level);
        }
        return result;
    }
};
