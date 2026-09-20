/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include <benchmark/benchmark.h>

#include "SpectrumAmplitudeRange.h"

#include <cmath>
#include <vector>

namespace
{
void runAutomaticAmplitudeRange (benchmark::State& state)
{
    constexpr std::size_t channelCount = 8;
    constexpr std::size_t columnCount = 1920;
    std::vector<std::vector<float>> means (
        channelCount, std::vector<float> (columnCount));
    std::vector<std::vector<float>> peaks (
        channelCount, std::vector<float> (columnCount));
    for (std::size_t channel = 0; channel < channelCount; ++channel)
        for (std::size_t column = 0; column < columnCount; ++column)
        {
            const auto value = -75.0f
                               + 10.0f * std::sin (0.01f * static_cast<float> (column))
                               + static_cast<float> (channel);
            means[channel][column] = value;
            peaks[channel][column] = value + (column == 1250 ? 40.0f : 3.0f);
        }

    spectrumviewer::SpectrumAmplitudeRange range;
    range.setMode (spectrumviewer::AmplitudeRangeMode::automatic);
    range.update (means, peaks, channelCount, 0.125);
    for (auto _ : state)
        benchmark::DoNotOptimize (range.update (means, peaks, channelCount, 0.125));
}

BENCHMARK (runAutomaticAmplitudeRange)
    ->Name ("GUI/AutoAmplitudeRange/8Channels/1920Columns")
    ->Unit (benchmark::kMicrosecond)
    ->UseRealTime();
} // namespace
