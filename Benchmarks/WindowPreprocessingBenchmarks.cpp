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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
enum class DetrendMode
{
    none = 0,
    mean = 1,
    linear = 2
};

enum class Implementation
{
    materialized,
    fused,
    tiled
};

template <typename Sample>
struct SplitView
{
    const Sample* first = nullptr;
    std::size_t firstSize = 0;
    const Sample* second = nullptr;
    std::size_t secondSize = 0;

    std::size_t size() const noexcept { return firstSize + secondSize; }
};

struct Trend
{
    double mean = 0.0;
    double slope = 0.0;
};

template <typename Sample>
Trend estimateTrend (const SplitView<Sample>& input, DetrendMode mode)
{
    if (mode == DetrendMode::none)
        return {};

    const auto count = input.size();
    const auto centre = 0.5 * static_cast<double> (count - 1);
    double sum = 0.0;
    double centredProduct = 0.0;
    double centredSquare = 0.0;
    std::size_t logicalIndex = 0;

    const auto accumulate = [&] (const Sample* samples, std::size_t numSamples)
    {
        for (std::size_t index = 0; index < numSamples; ++index, ++logicalIndex)
        {
            const auto value = static_cast<double> (samples[index]);
            const auto centredIndex = static_cast<double> (logicalIndex) - centre;
            sum += value;

            if (mode == DetrendMode::linear)
            {
                centredProduct += centredIndex * value;
                centredSquare += centredIndex * centredIndex;
            }
        }
    };

    accumulate (input.first, input.firstSize);
    accumulate (input.second, input.secondSize);

    Trend result;
    result.mean = sum / static_cast<double> (count);
    if (mode == DetrendMode::linear && centredSquare != 0.0)
        result.slope = centredProduct / centredSquare;

    return result;
}

template <typename Sample>
void materializeDetrendedRange (const SplitView<Sample>& input,
                                DetrendMode mode,
                                const Trend& trend,
                                std::size_t rangeStart,
                                std::size_t rangeSize,
                                Sample* destination)
{
    const auto centre = 0.5 * static_cast<double> (input.size() - 1);
    auto logicalIndex = rangeStart;
    const auto rangeEnd = rangeStart + rangeSize;

    while (logicalIndex < rangeEnd)
    {
        const auto inFirstRegion = logicalIndex < input.firstSize;
        const auto regionOffset = inFirstRegion ? logicalIndex : logicalIndex - input.firstSize;
        const auto regionSize = inFirstRegion ? input.firstSize : input.secondSize;
        const auto* source = (inFirstRegion ? input.first : input.second) + regionOffset;
        const auto count = std::min (rangeEnd - logicalIndex, regionSize - regionOffset);

        for (std::size_t index = 0; index < count; ++index, ++logicalIndex)
        {
            auto value = source[index];
            if (mode != DetrendMode::none)
                value -= static_cast<Sample> (trend.mean);
            if (mode == DetrendMode::linear)
                value -= static_cast<Sample> (trend.slope)
                       * static_cast<Sample> (static_cast<double> (logicalIndex) - centre);
            destination[logicalIndex - rangeStart] = value;
        }
    }
}

template <typename Sample>
void applyTaper (const Sample* input,
                 const Sample* taper,
                 Sample* destination,
                 std::size_t count)
{
    for (std::size_t index = 0; index < count; ++index)
        destination[index] = input[index] * taper[index];
}

template <typename Sample>
void detrendAndApplyTaper (const SplitView<Sample>& input,
                           DetrendMode mode,
                           const Trend& trend,
                           const Sample* taper,
                           Sample* destination)
{
    const auto centre = 0.5 * static_cast<double> (input.size() - 1);
    std::size_t logicalIndex = 0;

    const auto process = [&] (const Sample* samples, std::size_t numSamples)
    {
        for (std::size_t index = 0; index < numSamples; ++index, ++logicalIndex)
        {
            auto value = samples[index];
            if (mode != DetrendMode::none)
                value -= static_cast<Sample> (trend.mean);
            if (mode == DetrendMode::linear)
                value -= static_cast<Sample> (trend.slope)
                       * static_cast<Sample> (static_cast<double> (logicalIndex) - centre);
            destination[logicalIndex] = value * taper[logicalIndex];
        }
    };

    process (input.first, input.firstSize);
    process (input.second, input.secondSize);
}

template <typename Sample>
class Fixture
{
public:
    Fixture (std::size_t windowSize,
             std::size_t taperCount,
             std::size_t channelCount,
             std::size_t splitOffset,
             DetrendMode detrendMode)
        : windowSize (windowSize),
          taperCount (taperCount),
          channelCount (channelCount),
          splitOffset (splitOffset),
          detrendMode (detrendMode),
          history (windowSize * channelCount),
          tapers (windowSize * taperCount),
          detrended (windowSize * channelCount),
          tileScratch (std::min (windowSize, tileSize)),
          output (windowSize * taperCount * channelCount),
          trends (channelCount)
    {
        const auto pi = std::acos (-1.0);

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto* physical = history.data() + channel * windowSize;
            for (std::size_t logical = 0; logical < windowSize; ++logical)
            {
                const auto sample = 0.01 * static_cast<double> (logical)
                                  + std::sin (2.0 * pi * 17.0 * static_cast<double> (logical)
                                              / static_cast<double> (windowSize))
                                  + 0.1 * static_cast<double> (channel);
                physical[(splitOffset + logical) % windowSize] = static_cast<Sample> (sample);
            }
        }

        for (std::size_t taper = 0; taper < taperCount; ++taper)
        {
            auto* destination = tapers.data() + taper * windowSize;
            for (std::size_t sample = 0; sample < windowSize; ++sample)
            {
                destination[sample] = static_cast<Sample> (
                    std::sin (pi * static_cast<double> (taper + 1) * static_cast<double> (sample + 1)
                              / static_cast<double> (windowSize + 1)));
            }
        }
    }

    SplitView<Sample> getChannel (std::size_t channel) const noexcept
    {
        const auto* data = history.data() + channel * windowSize;
        return { data + splitOffset,
                 windowSize - splitOffset,
                 data,
                 splitOffset };
    }

    void runMaterialized()
    {
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto input = getChannel (channel);
            trends[channel] = estimateTrend (input, detrendMode);
            auto* detrendedChannel = detrended.data() + channel * windowSize;
            materializeDetrendedRange (input,
                                       detrendMode,
                                       trends[channel],
                                       0,
                                       windowSize,
                                       detrendedChannel);

            for (std::size_t taper = 0; taper < taperCount; ++taper)
            {
                applyTaper (detrendedChannel,
                            tapers.data() + taper * windowSize,
                            output.data() + (channel * taperCount + taper) * windowSize,
                            windowSize);
            }
        }
    }

    void runFused()
    {
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto input = getChannel (channel);
            trends[channel] = estimateTrend (input, detrendMode);

            for (std::size_t taper = 0; taper < taperCount; ++taper)
            {
                detrendAndApplyTaper (input,
                                      detrendMode,
                                      trends[channel],
                                      tapers.data() + taper * windowSize,
                                      output.data() + (channel * taperCount + taper) * windowSize);
            }
        }
    }

    void runTiled()
    {
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto input = getChannel (channel);
            trends[channel] = estimateTrend (input, detrendMode);

            for (std::size_t tileStart = 0; tileStart < windowSize; tileStart += tileSize)
            {
                const auto count = std::min (tileSize, windowSize - tileStart);
                materializeDetrendedRange (input,
                                           detrendMode,
                                           trends[channel],
                                           tileStart,
                                           count,
                                           tileScratch.data());

                for (std::size_t taper = 0; taper < taperCount; ++taper)
                {
                    applyTaper (tileScratch.data(),
                                tapers.data() + taper * windowSize + tileStart,
                                output.data() + (channel * taperCount + taper) * windowSize + tileStart,
                                count);
                }
            }
        }
    }

    const std::vector<Sample>& getOutput() const noexcept { return output; }

private:
    static constexpr std::size_t tileSize = 1024;

    const std::size_t windowSize;
    const std::size_t taperCount;
    const std::size_t channelCount;
    const std::size_t splitOffset;
    const DetrendMode detrendMode;
    std::vector<Sample> history;
    std::vector<Sample> tapers;
    std::vector<Sample> detrended;
    std::vector<Sample> tileScratch;
    std::vector<Sample> output;
    std::vector<Trend> trends;
};

DetrendMode getMode (std::int64_t value)
{
    if (value < 0 || value > 2)
        throw std::invalid_argument ("Unknown detrend mode");
    return static_cast<DetrendMode> (value);
}

template <typename Sample, Implementation implementation>
void runBenchmark (benchmark::State& state)
{
    const auto windowSize = static_cast<std::size_t> (state.range (0));
    const auto taperCount = static_cast<std::size_t> (state.range (1));
    const auto channelCount = static_cast<std::size_t> (state.range (2));
    const auto splitOffset = state.range (3) == 0 ? std::size_t { 0 } : windowSize / 3;
    Fixture<Sample> fixture (windowSize,
                             taperCount,
                             channelCount,
                             splitOffset,
                             getMode (state.range (4)));

    for (auto _ : state)
    {
        if constexpr (implementation == Implementation::materialized)
            fixture.runMaterialized();
        else if constexpr (implementation == Implementation::fused)
            fixture.runFused();
        else
            fixture.runTiled();

        benchmark::DoNotOptimize (fixture.getOutput().data());
        benchmark::ClobberMemory();
    }

    const auto processedSamples = static_cast<double> (windowSize * taperCount * channelCount);
    state.SetItemsProcessed (static_cast<std::int64_t> (state.iterations() * processedSamples));
    const auto scratchSamples = implementation == Implementation::materialized
                                  ? channelCount * windowSize
                                  : implementation == Implementation::tiled
                                      ? std::min (windowSize, std::size_t { 1024 })
                                      : std::size_t { 0 };
    state.counters["working_set_bytes"] = static_cast<double> (
        sizeof (Sample)
        * (windowSize * (channelCount + taperCount + channelCount * taperCount) + scratchSamples));
}

template <std::size_t numProfiles>
void addCases (benchmark::internal::Benchmark* benchmark,
               const std::int64_t (&profiles)[numProfiles][2])
{
    for (const auto& profile : profiles)
        for (const auto channels : { 1, 4, 8 })
            for (const auto wrapped : { 0, 1 })
                for (const auto mode : { 0, 1, 2 })
                    benchmark->Args ({ profile[0], profile[1], channels, wrapped, mode });

    benchmark->ArgNames ({ "N", "K", "channels", "wrapped", "detrend" })
             ->Unit (benchmark::kMicrosecond)
             ->UseRealTime()
             ->ComputeStatistics ("p99", [] (const std::vector<double>& values)
             {
                 auto sorted = values;
                 std::sort (sorted.begin(), sorted.end());
                 const auto index = static_cast<std::size_t> (
                     std::ceil (0.99 * static_cast<double> (sorted.size()))) - 1;
                 return sorted[index];
             });
}

void addLowRateCases (benchmark::internal::Benchmark* benchmark)
{
    constexpr std::int64_t profiles[][2] { { 500, 3 }, { 1000, 4 }, { 4000, 5 } };
    addCases (benchmark, profiles);
}

void addHighRateCases (benchmark::internal::Benchmark* benchmark)
{
    constexpr std::int64_t profiles[][2] { { 7500, 3 }, { 15000, 4 }, { 60000, 5 } };
    addCases (benchmark, profiles);
}

template <typename Sample>
bool validateEquivalentImplementations()
{
    constexpr std::size_t windowSize = 1000;
    constexpr std::size_t taperCount = 5;
    constexpr std::size_t channelCount = 3;

    for (const auto splitOffset : { std::size_t { 0 }, windowSize / 3 })
    {
        for (const auto mode : { DetrendMode::none, DetrendMode::mean, DetrendMode::linear })
        {
            Fixture<Sample> materialized (windowSize, taperCount, channelCount, splitOffset, mode);
            Fixture<Sample> fused (windowSize, taperCount, channelCount, splitOffset, mode);
            Fixture<Sample> tiled (windowSize, taperCount, channelCount, splitOffset, mode);
            materialized.runMaterialized();
            fused.runFused();
            tiled.runTiled();

            const auto& expected = materialized.getOutput();
            const auto& actual = fused.getOutput();
            const auto& tiledActual = tiled.getOutput();
            Sample maximumMagnitude = 0;
            Sample maximumDifference = 0;

            for (std::size_t index = 0; index < expected.size(); ++index)
            {
                maximumMagnitude = std::max (maximumMagnitude, std::abs (expected[index]));
                maximumDifference = std::max (maximumDifference,
                                              std::abs (expected[index] - actual[index]));
                maximumDifference = std::max (maximumDifference,
                                              std::abs (expected[index] - tiledActual[index]));
            }

            const auto tolerance = std::numeric_limits<Sample>::epsilon()
                                 * std::max (Sample { 1 }, maximumMagnitude) * Sample { 8 };
            if (maximumDifference > tolerance)
                return false;
        }
    }

    return true;
}

BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::materialized)->Name ("Float/Materialized/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::materialized)->Name ("Float/Materialized/30kHz")->Apply (addHighRateCases);
BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::fused)->Name ("Float/Fused/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::fused)->Name ("Float/Fused/30kHz")->Apply (addHighRateCases);
BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::tiled)->Name ("Float/Tiled/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, float, Implementation::tiled)->Name ("Float/Tiled/30kHz")->Apply (addHighRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::materialized)->Name ("Double/Materialized/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::materialized)->Name ("Double/Materialized/30kHz")->Apply (addHighRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::fused)->Name ("Double/Fused/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::fused)->Name ("Double/Fused/30kHz")->Apply (addHighRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::tiled)->Name ("Double/Tiled/2kHz")->Apply (addLowRateCases);
BENCHMARK_TEMPLATE (runBenchmark, double, Implementation::tiled)->Name ("Double/Tiled/30kHz")->Apply (addHighRateCases);
} // namespace

int main (int argc, char** argv)
{
    if (! validateEquivalentImplementations<float>()
        || ! validateEquivalentImplementations<double>())
    {
        std::cerr << "Preprocessing implementations differ" << std::endl;
        return 1;
    }

    benchmark::Initialize (&argc, argv);
    if (benchmark::ReportUnrecognizedArguments (argc, argv))
        return 1;

    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
