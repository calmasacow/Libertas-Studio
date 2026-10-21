// SPDX-FileCopyrightText: Libertas-Studio contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ltest.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <lcomp/audio_filters.hpp>
#include <lcomp/filters.hpp>

LTEST_CASE(audio_filters_process_real_blocks_and_preserve_disabled_samples) {
    for (const auto& info : lcomp::filter_catalog()) {
        if (!info.audio)
            continue;
        ldoc::Effect e;
        e.id = ldoc::Id::generate();
        e.type = info.type;
        for (const auto& field : info.fields)
            e.params[field.key] = field.value;
        std::array<float, 960> samples;
        samples.fill(0.25f);
        lcomp::AudioFilters processor;
        REQUIRE(processor.process(samples, std::span{&e, 1}));
        CHECK(std::ranges::all_of(samples, [](float s) { return std::isfinite(s); }));
        e.enabled = false;
        samples.fill(0.25f);
        REQUIRE(processor.process(samples, std::span{&e, 1}));
        CHECK(std::ranges::all_of(samples, [](float s) { return s == 0.25f; }));
    }
}

LTEST_CASE(audio_gain_polarity_limiter_and_sidechain_have_measured_effect) {
    lcomp::AudioFilters processor;
    ldoc::Effect e;
    e.id = ldoc::Id::generate();
    std::array<float, 960> samples;
    samples.fill(0.5f);
    e.type = "filter.gain";
    e.params["db"] = -6.020599913279624;
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK_NEAR(samples[100], 0.25, 1e-6);
    e.type = "filter.invert";
    e.params.clear();
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK_NEAR(samples[100], -0.25, 1e-6);
    e.type = "filter.limiter";
    e.params["threshold"] = -20.0;
    samples.fill(1);
    REQUIRE(processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return std::abs(s) <= 0.10001f; }));
    e.type = "filter.compressor";
    e.params.clear();
    e.params["threshold"] = -20.0;
    e.params["ratio"] = 10.0;
    e.params["attack_time"] = 0.0;
    const auto side = ldoc::Id::generate();
    e.params["sidechain"] = side.to_string();
    std::array<float, 960> detector;
    detector.fill(1);
    samples.fill(0.1f);
    REQUIRE(
        processor.process(samples, std::span{&e, 1}, [&](const auto& id) -> std::span<const float> {
            return id == side ? std::span<const float>{detector} : std::span<const float>{};
        }));
    CHECK(samples[100] < 0.02f);
    samples.fill(1);
    CHECK(!processor.process(samples, std::span{&e, 1}));
    CHECK(std::ranges::all_of(samples, [](float s) { return s == 0; }));
}

int main() {
    return ltest::run_all("filters");
}
