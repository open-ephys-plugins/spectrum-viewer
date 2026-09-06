/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "gtest/gtest.h"

#include "SpectrumAnalysis.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

constexpr double twoPi = 6.283185307179586476925286766559;

SpectrumAnalysisParameters makeParameters()
{
    SpectrumAnalysisParameters result;
    result.channelCount = 2;
    result.windowSampleCount = 8;
    result.hopSampleCount = 3;
    result.maximumInputBlockSampleCount = 5;
    result.sampleRateHz = 800.0;
    result.timeHalfBandwidth = 2.0;
    result.taperCount = 3;
    result.detrendMode = DetrendMode::mean;
    result.generation = 11;
    return result;
}

struct CapturedFrame
{
    std::int64_t firstSample = 0;
    std::uint64_t sequence = 0;
    spectrumviewer::SpectrumFrameDescriptor descriptor;
    std::vector<float> powers;
};

TEST (SpectrumAnalysisTests, RejectsInvalidOrFailedConfigurations)
{
    auto parameters = makeParameters();
    parameters.channelCount = 0;
    EXPECT_THROW ((SpectrumAnalysisConfiguration { parameters }), std::invalid_argument);

    parameters = makeParameters();
    parameters.timeHalfBandwidth = 4.0;
    EXPECT_THROW ((SpectrumAnalysisConfiguration { parameters }), std::invalid_argument);

    parameters = makeParameters();
    parameters.detrendMode = static_cast<DetrendMode> (99);
    EXPECT_THROW ((SpectrumAnalysisConfiguration { parameters }), std::invalid_argument);

    EXPECT_THROW ((SpectrumAnalysisPipeline { nullptr }), std::invalid_argument);
}

TEST (SpectrumAnalysisTests, ProducesSampleIndexedFramesIndependentOfCallbackPartitioning)
{
    const auto parameters = makeParameters();
    auto configuration = std::make_shared<const SpectrumAnalysisConfiguration> (parameters);
    SpectrumAnalysisPipeline pipeline (configuration);

    constexpr std::size_t totalSamples = 17;
    std::array<std::vector<float>, 2> samples {
        std::vector<float> (totalSamples), std::vector<float> (totalSamples)
    };
    for (std::size_t sample = 0; sample < totalSamples; ++sample)
    {
        const auto phase = twoPi * static_cast<double> (sample) / 8.0;
        samples[0][sample] = static_cast<float> (4.0 + std::cos (phase));
        samples[1][sample] = static_cast<float> (-3.0 + 2.0 * std::sin (2.0 * phase));
    }

    std::vector<CapturedFrame> frames;
    std::size_t offset = 0;
    for (const auto blockSize : { 2u, 5u, 1u, 4u, 5u })
    {
        const std::array<const float*, 2> block {
            samples[0].data() + offset, samples[1].data() + offset
        };
        const auto append = pipeline.appendBlock (block.data(),
                                                  block.size(),
                                                  blockSize,
                                                  100 + static_cast<std::int64_t> (offset),
                                                  parameters.generation);
        ASSERT_EQ (append.status, SpectrumAnalysisPipeline::AppendStatus::accepted);
        offset += blockSize;
        pipeline.consumeReadyFrames ([&] (const auto& frame)
                                     {
            CapturedFrame captured;
            captured.firstSample = frame.firstSample;
            captured.sequence = frame.sequence;
            captured.descriptor = frame.descriptor;
            captured.powers.assign (frame.getChannelData (0),
                                    frame.getChannelData (0) + frame.numChannels * frame.numBins);
            frames.push_back (std::move (captured)); });
    }

    ASSERT_EQ (frames.size(), 4u);
    MultitaperPeriodogram reference (parameters.channelCount,
                                     parameters.sampleRateHz,
                                     configuration->getTaperBank(),
                                     parameters.detrendMode,
                                     parameters.estimatorPlanningFlags);
    for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
    {
        const auto sampleOffset = frameIndex * parameters.hopSampleCount;
        const std::array<ChannelSampleView, 2> views {
            ChannelSampleView { samples[0].data() + sampleOffset,
                                parameters.windowSampleCount,
                                nullptr,
                                0 },
            ChannelSampleView { samples[1].data() + sampleOffset,
                                parameters.windowSampleCount,
                                nullptr,
                                0 }
        };
        ASSERT_TRUE (reference.compute (views.data(), views.size()));
        EXPECT_EQ (frames[frameIndex].firstSample,
                   100 + static_cast<std::int64_t> (sampleOffset));
        EXPECT_EQ (frames[frameIndex].sequence, frameIndex);
        EXPECT_EQ (frames[frameIndex].descriptor.configurationGeneration, 11u);
        EXPECT_DOUBLE_EQ (frames[frameIndex].descriptor.sampleRateHz, 800.0);
        EXPECT_DOUBLE_EQ (frames[frameIndex].descriptor.binWidthHz, 100.0);
        for (std::size_t channel = 0; channel < parameters.channelCount; ++channel)
        {
            for (std::size_t bin = 0; bin < reference.getBinCount(); ++bin)
            {
                EXPECT_FLOAT_EQ (
                    frames[frameIndex].powers[channel * reference.getBinCount() + bin],
                    reference.getChannelData (channel)[bin]);
            }
        }
    }
}

TEST (SpectrumAnalysisTests, RejectsForeignGenerationsWithoutMixingThemIntoHistory)
{
    const auto parameters = makeParameters();
    auto configuration = std::make_shared<const SpectrumAnalysisConfiguration> (parameters);
    SpectrumAnalysisPipeline pipeline (configuration);
    std::array<float, 8> samples { 0, 1, 2, 3, 4, 5, 6, 7 };
    const std::array<const float*, 2> first { samples.data(), samples.data() };
    ASSERT_EQ (pipeline.appendBlock (first.data(), 2, 4, 0, parameters.generation).status,
               SpectrumAnalysisPipeline::AppendStatus::accepted);

    const std::array<const float*, 2> second { samples.data() + 4, samples.data() + 4 };
    EXPECT_EQ (pipeline.appendBlock (second.data(), 2, 4, 4, parameters.generation + 1).status,
               SpectrumAnalysisPipeline::AppendStatus::configurationMismatch);
    EXPECT_EQ (pipeline.consumeReadyFrames ([] (const auto&) {}), 0u);

    ASSERT_EQ (pipeline.appendBlock (second.data(), 2, 4, 4, parameters.generation).status,
               SpectrumAnalysisPipeline::AppendStatus::accepted);
    std::size_t frames = 0;
    pipeline.consumeReadyFrames ([&] (const auto& frame)
                                 {
        EXPECT_EQ (frame.firstSample, 0);
        ++frames; });
    EXPECT_EQ (frames, 1u);
}

TEST (SpectrumAnalysisTests, ResetsPartialHistoryAtSampleDiscontinuities)
{
    auto parameters = makeParameters();
    parameters.maximumInputBlockSampleCount = 8;
    auto configuration = std::make_shared<const SpectrumAnalysisConfiguration> (parameters);
    SpectrumAnalysisPipeline pipeline (configuration);
    std::array<float, 12> samples {};
    const std::array<const float*, 2> first { samples.data(), samples.data() };
    ASSERT_EQ (pipeline.appendBlock (first.data(), 2, 4, 0, parameters.generation).status,
               SpectrumAnalysisPipeline::AppendStatus::accepted);

    const std::array<const float*, 2> second { samples.data() + 4, samples.data() + 4 };
    const auto append = pipeline.appendBlock (second.data(), 2, 8, 10, parameters.generation);
    ASSERT_EQ (append.status, SpectrumAnalysisPipeline::AppendStatus::accepted);
    EXPECT_TRUE (append.discontinuity);
    EXPECT_EQ (pipeline.getDiscontinuityCount(), 1u);

    std::size_t frames = 0;
    pipeline.consumeReadyFrames ([&] (const auto& frame)
                                 {
        EXPECT_EQ (frame.firstSample, 10);
        ++frames; });
    EXPECT_EQ (frames, 1u);
}

TEST (SpectrumAnalysisTests, CountsFailedWindowsAndLeavesASequenceGap)
{
    auto parameters = makeParameters();
    parameters.hopSampleCount = parameters.windowSampleCount;
    parameters.maximumInputBlockSampleCount = parameters.windowSampleCount;
    auto configuration = std::make_shared<const SpectrumAnalysisConfiguration> (parameters);
    SpectrumAnalysisPipeline pipeline (configuration);
    std::array<float, 16> samples {};
    samples[3] = std::numeric_limits<float>::quiet_NaN();
    const std::array<const float*, 2> first { samples.data(), samples.data() };
    ASSERT_EQ (pipeline.appendBlock (first.data(), 2, 8, 0, parameters.generation).status,
               SpectrumAnalysisPipeline::AppendStatus::accepted);
    EXPECT_EQ (pipeline.consumeReadyFrames ([] (const auto&) {}), 0u);
    EXPECT_EQ (pipeline.getFailedWindowCount(), 1u);

    const std::array<const float*, 2> second { samples.data() + 8, samples.data() + 8 };
    ASSERT_EQ (pipeline.appendBlock (second.data(), 2, 8, 8, parameters.generation).status,
               SpectrumAnalysisPipeline::AppendStatus::accepted);
    std::size_t frames = 0;
    pipeline.consumeReadyFrames ([&] (const auto& frame)
                                 {
        EXPECT_EQ (frame.firstSample, 8);
        EXPECT_EQ (frame.sequence, 1u);
        ++frames; });
    EXPECT_EQ (frames, 1u);
}
} // namespace
