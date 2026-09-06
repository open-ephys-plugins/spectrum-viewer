/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <benchmark/benchmark.h>

#include "DpssTapers.h"
#include "MultitaperPeriodogram.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::ChannelSampleView;
using spectrumviewer::DetrendMode;
using spectrumviewer::MultitaperPeriodogram;

constexpr double twoPi = 6.283185307179586476925286766559;
constexpr unsigned int fftwEstimate = 1U << 6U;

DetrendMode getMode (std::int64_t value)
{
    if (value < 0 || value > 2)
        throw std::invalid_argument ("Unknown detrend mode");
    return static_cast<DetrendMode> (value);
}

void runMultitaperPeriodogram (benchmark::State& state)
{
    const auto sampleCount = static_cast<std::size_t> (state.range (0));
    const auto taperCount = static_cast<std::size_t> (state.range (1));
    const auto twiceNw = static_cast<double> (state.range (2));
    const auto channelCount = static_cast<std::size_t> (state.range (3));
    const auto mode = getMode (state.range (4));

    auto mutableBank = spectrumviewer::generateDpssTapers (
        sampleCount, 0.5 * twiceNw, taperCount, fftwEstimate);
    if (! mutableBank.succeeded())
    {
        state.SkipWithError ("Unable to generate DPSS bank");
        return;
    }
    auto bank = std::make_shared<const spectrumviewer::DpssTaperBank> (
        std::move (mutableBank));
    MultitaperPeriodogram estimator (
        channelCount, 30000.0, bank, mode, fftwEstimate);
    std::vector<float> samples (sampleCount * channelCount);
    std::vector<ChannelSampleView> views (channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        auto* output = samples.data() + channel * sampleCount;
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            output[sample] = static_cast<float> (
                100.0 + 0.001 * static_cast<double> (sample)
                + std::sin (twoPi * 173.25 * static_cast<double> (sample) / 30000.0)
                + 0.1 * static_cast<double> (channel));
        }
        views[channel] = { output, sampleCount, nullptr, 0 };
    }

    for (auto _ : state)
    {
        if (! estimator.compute (views.data(), views.size()))
            state.SkipWithError ("Estimator rejected benchmark input");
        benchmark::DoNotOptimize (estimator.getChannelData (0));
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (
        state.iterations() * static_cast<std::int64_t> (channelCount));
    state.counters["channels"] = static_cast<double> (channelCount);
    state.counters["transforms"] = static_cast<double> (channelCount * taperCount);
    state.counters["input_bytes"] = static_cast<double> (
        sizeof (float) * sampleCount * channelCount);
}

void addMultitaperCases (benchmark::internal::Benchmark* benchmark)
{
    for (const auto& profile : {
             std::vector<std::int64_t> { 7500, 3, 4 },
             std::vector<std::int64_t> { 15000, 4, 5 },
             std::vector<std::int64_t> { 60000, 5, 6 } })
    {
        for (const auto channels : { 1, 4, 8 })
            for (const auto mode : { 0, 1, 2 })
                benchmark->Args ({ profile[0], profile[1], profile[2], channels, mode });
    }

    benchmark->ArgNames ({ "N", "K", "2NW", "channels", "detrend" })
        ->Unit (benchmark::kMicrosecond)
        ->UseRealTime();
}

BENCHMARK (runMultitaperPeriodogram)
    ->Name ("Estimator/FloatEqualMultitaper")
    ->Apply (addMultitaperCases);
} // namespace
