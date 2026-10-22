// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <format>
#include <ldoc/document.hpp>

namespace lcomp {
inline std::int64_t timer_number(const ldoc::ParamMap& settings, const char* key,
                                 std::int64_t fallback = 0) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<std::int64_t>(&it->second);
    return value ? *value : fallback;
}

inline bool timer_flag(const ldoc::ParamMap& settings, const char* key, bool fallback = false) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<bool>(&it->second);
    return value ? *value : fallback;
}

inline std::string timer_text(const ldoc::ParamMap& settings, const char* key,
                              std::string fallback = {}) {
    const auto it = settings.find(key);
    const auto* value = it == settings.end() ? nullptr : std::get_if<std::string>(&it->second);
    return value ? *value : fallback;
}

struct CountdownValue {
    std::int64_t elapsed = 0, remaining = 0;
    bool ended = false;
    std::string text;
};

inline CountdownValue countdown(const ldoc::ParamMap& settings, std::int64_t now) {
    constexpr std::int64_t limit = 366ll * 86400 * 1000;
    const auto duration =
        std::clamp(timer_number(settings, "countdown.durationMs", 900000), std::int64_t{0}, limit);
    const auto offset =
        std::clamp(timer_number(settings, "countdown.elapsedMs"), std::int64_t{0}, limit);
    const auto mode = timer_text(settings, "countdown.mode", "remaining");
    const bool running = timer_flag(settings, "countdown.running");
    const auto start = std::clamp(timer_number(settings, "countdown.startedAtMs", now),
                                  std::int64_t{0}, std::int64_t{4102444800000});
    CountdownValue out;
    out.elapsed =
        std::clamp(offset + (running ? std::clamp(now - start, std::int64_t{0}, limit) : 0),
                   std::int64_t{0}, limit);
    out.remaining = std::max<std::int64_t>(0, duration - out.elapsed);
    if (mode == "target")
        out.remaining =
            running ? std::clamp(std::clamp(timer_number(settings, "countdown.targetAtMs", now),
                                            std::int64_t{0}, std::int64_t{4102444800000}) -
                                     now,
                                 std::int64_t{0}, limit)
                    : std::clamp(timer_number(settings, "countdown.targetRemainingMs", duration),
                                 std::int64_t{0}, limit);
    out.ended = mode == "elapsed" ? duration > 0 && out.elapsed >= duration : out.remaining == 0;
    auto seconds = mode == "elapsed" ? std::min(out.elapsed, duration ? duration : limit) / 1000
                                     : (out.remaining + 999) / 1000;
    const bool days = timer_flag(settings, "countdown.showDays");
    const bool hours = timer_flag(settings, "countdown.showHours");
    const bool minutes = timer_flag(settings, "countdown.showMinutes", true);
    const bool zero = timer_flag(settings, "countdown.leadingZero", true);
    std::string time;
    const auto append = [&](std::int64_t value) {
        const bool first = time.empty();
        if (!first)
            time += ':';
        time += first && !zero ? std::to_string(value) : std::format("{:02}", value);
    };
    if (days)
        append(seconds / 86400);
    if (hours)
        append(seconds / 3600 % 24);
    if (minutes)
        append(seconds / 60 % 60);
    if (timer_flag(settings, "countdown.showSeconds", true))
        append(seconds % 60);
    out.text = timer_text(settings, "countdown.format", "%time%");
    for (std::size_t at = 0; (at = out.text.find("%time%", at)) != std::string::npos;
         at += time.size())
        out.text.replace(at, 6, time);
    if (out.ended && timer_flag(settings, "countdown.showEndMessage", true))
        out.text = timer_text(settings, "countdown.endMessage", "SOON");
    return out;
}
} // namespace lcomp
