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
#include "SpectrumAnalysis.h"
#include "SpectrumDisplayReducer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::ChannelSampleView;
using spectrumviewer::DetrendMode;
using spectrumviewer::MultitaperPeriodogram;
using spectrumviewer::SpectrumAnalysisConfiguration;
using spectrumviewer::SpectrumAnalysisParameters;
using spectrumviewer::SpectrumAnalysisPipeline;
using spectrumviewer::SpectrumDisplayReducer;

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

template <bool withDisplayReduction>
void runSpectrumAnalysisPipeline (benchmark::State& state)
{
    const auto sampleCount = static_cast<std::size_t> (state.range (0));
    const auto taperCount = static_cast<std::size_t> (state.range (1));
    const auto twiceNw = static_cast<double> (state.range (2));
    const auto channelCount = static_cast<std::size_t> (state.range (3));
    const auto mode = getMode (state.range (4));
    const auto hopSampleCount = sampleCount == 60000 ? sampleCount / 4 : sampleCount / 2;

    SpectrumAnalysisParameters parameters;
    parameters.channelCount = channelCount;
    parameters.windowSampleCount = sampleCount;
    parameters.hopSampleCount = hopSampleCount;
    parameters.maximumInputBlockSampleCount = hopSampleCount;
    parameters.sampleRateHz = 30000.0;
    parameters.timeHalfBandwidth = 0.5 * twiceNw;
    parameters.taperCount = taperCount;
    parameters.detrendMode = mode;
    parameters.generation = 1;
    auto configuration = std::make_shared<const SpectrumAnalysisConfiguration> (parameters);
    SpectrumAnalysisPipeline pipeline (configuration);
    constexpr std::size_t displayColumns = 1920;
    SpectrumDisplayReducer reducer (channelCount,
                                    configuration->getBinCount(),
                                    displayColumns);
    std::vector<float> publishedMeans (channelCount * displayColumns);
    std::vector<float> publishedPeaks (channelCount * displayColumns);
    std::vector<float> publishedFrequencies (displayColumns);

    std::vector<float> samples (hopSampleCount * channelCount);
    std::vector<const float*> channels (channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        auto* output = samples.data() + channel * hopSampleCount;
        channels[channel] = output;
        for (std::size_t sample = 0; sample < hopSampleCount; ++sample)
        {
            output[sample] = static_cast<float> (
                100.0 + std::sin (twoPi * 173.25 * static_cast<double> (sample) / 30000.0)
                + 0.1 * static_cast<double> (channel));
        }
    }

    std::size_t prefilledSamples = 0;
    while (prefilledSamples < sampleCount - hopSampleCount)
    {
        const auto prefill = pipeline.appendBlock (
            channels.data(),
            channelCount,
            hopSampleCount,
            static_cast<std::int64_t> (prefilledSamples),
            parameters.generation);
        if (prefill.status != SpectrumAnalysisPipeline::AppendStatus::accepted)
        {
            state.SkipWithError ("Pipeline rejected prefill block");
            return;
        }
        prefilledSamples += hopSampleCount;
    }
    auto nextSample = static_cast<std::int64_t> (prefilledSamples);

    for (auto _ : state)
    {
        const auto append = pipeline.appendBlock (channels.data(),
                                                  channelCount,
                                                  hopSampleCount,
                                                  nextSample,
                                                  parameters.generation);
        if (append.status != SpectrumAnalysisPipeline::AppendStatus::accepted)
        {
            state.SkipWithError ("Pipeline rejected benchmark block");
            break;
        }

        std::size_t frameCount = 0;
        pipeline.consumeReadyFrames ([&] (const auto& frame)
                                     {
            if constexpr (withDisplayReduction)
            {
                const auto scale = state.range (5) == 0
                                       ? spectrumviewer::FrequencyScale::linear
                                       : spectrumviewer::FrequencyScale::logarithmic;
                if (! reducer.reduce (frame.getChannelData (0), frame.numChannels,
                                      frame.numBins, frame.descriptor.sampleRateHz,
                                      frame.descriptor.windowSampleCount, displayColumns,
                                      scale, 0.0, frame.descriptor.sampleRateHz * 0.5))
                {
                    state.SkipWithError ("Display reducer rejected benchmark frame");
                    return;
                }
                const auto& display = reducer.getView();
                for (std::size_t channel = 0; channel < channelCount; ++channel)
                {
                    std::memcpy (publishedMeans.data() + channel * display.numColumns,
                                 display.getChannelMean (channel),
                                 display.numColumns * sizeof (float));
                    std::memcpy (publishedPeaks.data() + channel * display.numColumns,
                                 display.getChannelPeak (channel),
                                 display.numColumns * sizeof (float));
                }
                std::memcpy (publishedFrequencies.data(), display.frequenciesHz,
                             display.numColumns * sizeof (float));
                benchmark::DoNotOptimize (publishedMeans.data());
                benchmark::DoNotOptimize (publishedPeaks.data());
            }
            else
                benchmark::DoNotOptimize (frame.getChannelData (0));
            ++frameCount; });
        if (frameCount != 1)
        {
            state.SkipWithError ("Pipeline did not produce exactly one frame");
            break;
        }
        nextSample += static_cast<std::int64_t> (hopSampleCount);
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (
        state.iterations() * static_cast<std::int64_t> (channelCount));
    state.SetBytesProcessed (
        state.iterations() * static_cast<std::int64_t> (sizeof (float) * hopSampleCount * channelCount));
}

void addDisplayPipelineCases (benchmark::internal::Benchmark* benchmark)
{
    for (const auto& profile : {
             std::vector<std::int64_t> { 7500, 3, 4 },
             std::vector<std::int64_t> { 15000, 4, 5 },
             std::vector<std::int64_t> { 60000, 5, 6 } })
        for (const auto scale : { 0, 1 })
            benchmark->Args ({ profile[0], profile[1], profile[2], 8, 2, scale });

    benchmark->ArgNames ({ "N", "K", "2NW", "channels", "detrend", "log_axis" })
        ->Unit (benchmark::kMicrosecond)
        ->UseRealTime();
}

BENCHMARK (runMultitaperPeriodogram)
    ->Name ("Estimator/FloatEqualMultitaper")
    ->Apply (addMultitaperCases);

BENCHMARK_TEMPLATE (runSpectrumAnalysisPipeline, false)
    ->Name ("Pipeline/FloatEqualMultitaper")
    ->Apply (addMultitaperCases);

BENCHMARK_TEMPLATE (runSpectrumAnalysisPipeline, true)
    ->Name ("WorkerToDisplay/FloatEqualMultitaper")
    ->Apply (addDisplayPipelineCases);
} // namespace
