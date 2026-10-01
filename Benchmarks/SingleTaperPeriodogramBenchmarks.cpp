/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <benchmark/benchmark.h>

#include "SingleTaperPeriodogram.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::ChannelSampleView;
using spectrumviewer::DetrendMode;
using spectrumviewer::SingleTaperPeriodogram;

constexpr double twoPi = 6.283185307179586476925286766559;

class Fixture
{
public:
    Fixture (std::size_t numSamples,
             std::size_t numChannels,
             bool wrapped,
             DetrendMode mode)
        : sampleCount (numSamples),
          channelCount (numChannels),
          splitOffset (wrapped ? numSamples / 3 : 0),
          samples (numSamples * numChannels),
          views (numChannels),
          estimator (numChannels, numSamples, 30000.0, makeTaper (numSamples), mode)
    {
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto* physical = samples.data() + channel * sampleCount;
            for (std::size_t logical = 0; logical < sampleCount; ++logical)
            {
                const auto value = 100.0 + 0.001 * static_cast<double> (logical)
                                   + std::sin (twoPi * 173.25 * static_cast<double> (logical)
                                               / static_cast<double> (sampleCount))
                                   + 0.1 * static_cast<double> (channel);
                physical[(splitOffset + logical) % sampleCount] = static_cast<float> (value);
            }

            views[channel] = { physical + splitOffset,
                               sampleCount - splitOffset,
                               splitOffset == 0 ? nullptr : physical,
                               splitOffset };
        }
    }

    bool compute() { return estimator.compute (views.data(), views.size()); }
    const float* getOutput() const noexcept { return estimator.getChannelData (0); }

private:
    static std::vector<float> makeTaper (std::size_t size)
    {
        std::vector<float> result (size);
        for (std::size_t sample = 0; sample < size; ++sample)
        {
            result[sample] = static_cast<float> (
                0.54 - 0.46 * std::cos (twoPi * static_cast<double> (sample) / static_cast<double> (size - 1)));
        }
        return result;
    }

    const std::size_t sampleCount;
    const std::size_t channelCount;
    const std::size_t splitOffset;
    std::vector<float> samples;
    std::vector<ChannelSampleView> views;
    SingleTaperPeriodogram estimator;
};

DetrendMode getMode (std::int64_t value)
{
    if (value < 0 || value > 2)
        throw std::invalid_argument ("Unknown detrend mode");
    return static_cast<DetrendMode> (value);
}

void runSingleTaperPeriodogram (benchmark::State& state)
{
    const auto samples = static_cast<std::size_t> (state.range (0));
    const auto channels = static_cast<std::size_t> (state.range (1));
    Fixture fixture (samples, channels, state.range (2) != 0, getMode (state.range (3)));

    for (auto _ : state)
    {
        if (! fixture.compute())
            state.SkipWithError ("Estimator rejected benchmark input");
        benchmark::DoNotOptimize (fixture.getOutput());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (state.iterations() * static_cast<std::int64_t> (channels));
    state.counters["channels"] = static_cast<double> (channels);
    state.counters["input_bytes"] = static_cast<double> (sizeof (float) * samples * channels);
}

void addEstimatorCases (benchmark::internal::Benchmark* benchmark)
{
    for (const auto samples : { 7500, 15000, 60000 })
        for (const auto channels : { 1, 4, 8 })
            for (const auto wrapped : { 0, 1 })
                for (const auto mode : { 0, 1, 2 })
                    benchmark->Args ({ samples, channels, wrapped, mode });

    benchmark->ArgNames ({ "N", "channels", "wrapped", "detrend" })
        ->Unit (benchmark::kMicrosecond)
        ->UseRealTime();
}

BENCHMARK (runSingleTaperPeriodogram)->Name ("Estimator/FloatSingleTaper")->Apply (addEstimatorCases);
} // namespace
