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

#include "gtest/gtest.h"

#include "DpssTapers.h"
#include "MultitaperPeriodogram.h"
#include "ReferencePeriodogram.h"
#include "SingleTaperPeriodogram.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::ChannelSampleView;
using spectrumviewer::DetrendMode;
using spectrumviewer::DpssGenerationStatus;
using spectrumviewer::DpssTaperBank;
using spectrumviewer::MultitaperPeriodogram;
using spectrumviewer::ReferencePeriodogram;
using spectrumviewer::SingleTaperPeriodogram;

constexpr double twoPi = 6.283185307179586476925286766559;
constexpr unsigned int fftwEstimate = 1U << 6U;

std::shared_ptr<const DpssTaperBank> generateBank (std::size_t sampleCount,
                                                   double timeHalfBandwidth,
                                                   std::size_t taperCount)
{
    return std::make_shared<const DpssTaperBank> (
        spectrumviewer::generateDpssTapers (
            sampleCount, timeHalfBandwidth, taperCount, fftwEstimate));
}

std::vector<double> referenceAverage (const float* samples,
                                      std::size_t channelCount,
                                      std::size_t stride,
                                      double sampleRate,
                                      const DpssTaperBank& bank,
                                      DetrendMode mode)
{
    const auto binCount = bank.sampleCount / 2 + 1;
    std::vector<double> result (channelCount * binCount, 0.0);
    for (std::size_t taper = 0; taper < bank.taperCount; ++taper)
    {
        const auto* first = bank.getTaper (taper);
        const std::vector<double> coefficients (first, first + bank.sampleCount);
        const auto estimate = ReferencePeriodogram::compute (
            samples,
            channelCount,
            stride,
            bank.sampleCount,
            sampleRate,
            coefficients,
            mode);
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] += estimate.psd[index] / static_cast<double> (bank.taperCount);
    }
    return result;
}

void expectMatchesReference (std::size_t sampleCount, DetrendMode mode)
{
    constexpr std::size_t channelCount = 3;
    const auto stride = sampleCount + 7;
    std::vector<float> planar (channelCount * stride, -12345.0f);
    std::mt19937 generator (static_cast<unsigned int> (8173 + sampleCount));
    std::normal_distribution<float> noise (0.0f, 0.3f);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        auto* output = planar.data() + channel * stride;
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            const auto phase = twoPi * static_cast<double> (sample)
                               / static_cast<double> (sampleCount);
            output[sample] = static_cast<float> (
                                 50.0 + 0.02 * static_cast<double> (sample)
                                 + (1.0 + static_cast<double> (channel))
                                       * std::cos (static_cast<double> (2 * channel + 3) * phase))
                             + noise (generator);
        }
    }

    const auto bank = generateBank (sampleCount, 2.5, 4);
    ASSERT_TRUE (bank->succeeded());
    const auto expected = referenceAverage (
        planar.data(), channelCount, stride, 30000.0, *bank, mode);
    MultitaperPeriodogram estimator (
        channelCount, 30000.0, bank, mode, fftwEstimate);
    std::array<ChannelSampleView, channelCount> views;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
        views[channel] = { planar.data() + channel * stride, sampleCount, nullptr, 0 };

    ASSERT_TRUE (estimator.compute (views.data(), views.size()));
    ASSERT_EQ (estimator.getBinCount(), sampleCount / 2 + 1);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto* actual = estimator.getChannelData (channel);
        for (std::size_t bin = 0; bin < estimator.getBinCount(); ++bin)
        {
            const auto reference = expected[channel * estimator.getBinCount() + bin];
            const auto tolerance = std::max (1.0e-10, std::abs (reference) * 4.0e-4);
            EXPECT_NEAR (static_cast<double> (actual[bin]), reference, tolerance)
                << "channel=" << channel << " bin=" << bin
                << " N=" << sampleCount;
        }
    }
}

TEST (MultitaperPeriodogramTests, MatchesDoubleOracleAcrossLengthsAndDetrendingModes)
{
    for (const auto sampleCount : { 15u, 16u, 31u, 64u })
        for (const auto mode : { DetrendMode::none, DetrendMode::mean, DetrendMode::linear })
            expectMatchesReference (sampleCount, mode);
}

TEST (MultitaperPeriodogramTests, SplitAndContiguousViewsProduceIdenticalOutput)
{
    constexpr std::size_t sampleCount = 127;
    constexpr std::size_t split = 39;
    std::vector<float> contiguous (sampleCount);
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        contiguous[sample] = static_cast<float> (
            5.0 + 0.01 * static_cast<double> (sample)
            + std::cos (twoPi * 13.2 * static_cast<double> (sample)
                        / static_cast<double> (sampleCount)));
    }
    std::vector<float> circular (sampleCount);
    std::copy (contiguous.begin(),
               contiguous.end() - static_cast<std::ptrdiff_t> (split),
               circular.begin() + split);
    std::copy (contiguous.end() - static_cast<std::ptrdiff_t> (split),
               contiguous.end(),
               circular.begin());

    const auto bank = generateBank (sampleCount, 2.5, 4);
    ASSERT_TRUE (bank->succeeded());
    MultitaperPeriodogram contiguousEstimator (
        1, 30000.0, bank, DetrendMode::linear, fftwEstimate);
    MultitaperPeriodogram splitEstimator (
        1, 30000.0, bank, DetrendMode::linear, fftwEstimate);
    const ChannelSampleView contiguousView {
        contiguous.data(), sampleCount, nullptr, 0
    };
    const ChannelSampleView splitView {
        circular.data() + split, sampleCount - split, circular.data(), split
    };
    ASSERT_TRUE (contiguousEstimator.compute (&contiguousView, 1));
    ASSERT_TRUE (splitEstimator.compute (&splitView, 1));

    for (std::size_t bin = 0; bin < contiguousEstimator.getBinCount(); ++bin)
        EXPECT_FLOAT_EQ (contiguousEstimator.getChannelData (0)[bin],
                         splitEstimator.getChannelData (0)[bin]);
}

TEST (MultitaperPeriodogramTests, OneTaperMatchesSingleTaperEstimator)
{
    constexpr std::size_t sampleCount = 128;
    std::vector<float> samples (sampleCount);
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        const auto phase = twoPi * static_cast<double> (sample)
                           / static_cast<double> (sampleCount);
        samples[sample] = static_cast<float> (
            1000.0 + 0.05 * static_cast<double> (sample)
            + 4.0 * std::cos (17.0 * phase));
    }

    const auto bank = generateBank (sampleCount, 2.0, 1);
    ASSERT_TRUE (bank->succeeded());
    const std::vector<float> taper (
        bank->getTaper (0), bank->getTaper (0) + sampleCount);
    SingleTaperPeriodogram single (
        1, sampleCount, 30000.0, taper, DetrendMode::linear, fftwEstimate);
    MultitaperPeriodogram multi (
        1, 30000.0, bank, DetrendMode::linear, fftwEstimate);
    const ChannelSampleView view { samples.data(), sampleCount, nullptr, 0 };
    ASSERT_TRUE (single.compute (&view, 1));
    ASSERT_TRUE (multi.compute (&view, 1));

    for (std::size_t bin = 0; bin < multi.getBinCount(); ++bin)
        EXPECT_FLOAT_EQ (multi.getChannelData (0)[bin], single.getChannelData (0)[bin]);
}

TEST (MultitaperPeriodogramTests, DoesNotDoubleDcOrNyquistEndpoints)
{
    constexpr std::size_t sampleCount = 128;
    constexpr double sampleRate = 30000.0;
    constexpr float amplitude = 2.5f;
    const auto bank = generateBank (sampleCount, 2.5, 4);
    ASSERT_TRUE (bank->succeeded());

    std::array<std::vector<float>, 2> samples {
        std::vector<float> (sampleCount, amplitude), std::vector<float> (sampleCount)
    };
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
        samples[1][sample] = sample % 2 == 0 ? amplitude : -amplitude;

    double expectedEndpoint = 0.0;
    for (std::size_t taper = 0; taper < bank->taperCount; ++taper)
    {
        const auto* coefficients = bank->getTaper (taper);
        double sum = 0.0;
        double energy = 0.0;
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            sum += coefficients[sample];
            energy += static_cast<double> (coefficients[sample]) * coefficients[sample];
        }
        expectedEndpoint += static_cast<double> (amplitude) * amplitude * sum * sum
                            / (sampleRate * energy * static_cast<double> (bank->taperCount));
    }

    MultitaperPeriodogram estimator (
        2, sampleRate, bank, DetrendMode::none, fftwEstimate);
    const std::array<ChannelSampleView, 2> views {
        ChannelSampleView { samples[0].data(), sampleCount, nullptr, 0 },
        ChannelSampleView { samples[1].data(), sampleCount, nullptr, 0 }
    };
    ASSERT_TRUE (estimator.compute (views.data(), views.size()));

    EXPECT_NEAR (estimator.getChannelData (0)[0], expectedEndpoint, expectedEndpoint * 1.0e-5);
    EXPECT_NEAR (estimator.getChannelData (1)[sampleCount / 2],
                 expectedEndpoint,
                 expectedEndpoint * 1.0e-5);
}

TEST (MultitaperPeriodogramTests, KeepsPlanarChannelsIsolated)
{
    constexpr std::size_t sampleCount = 256;
    std::array<std::vector<float>, 2> samples {
        std::vector<float> (sampleCount), std::vector<float> (sampleCount, 0.0f)
    };
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        samples[0][sample] = static_cast<float> (
            std::cos (twoPi * 23.0 * static_cast<double> (sample)
                      / static_cast<double> (sampleCount)));
    }

    const auto bank = generateBank (sampleCount, 3.0, 5);
    ASSERT_TRUE (bank->succeeded());
    MultitaperPeriodogram estimator (
        2, 30000.0, bank, DetrendMode::mean, fftwEstimate);
    const std::array<ChannelSampleView, 2> views {
        ChannelSampleView { samples[0].data(), sampleCount, nullptr, 0 },
        ChannelSampleView { samples[1].data(), sampleCount, nullptr, 0 }
    };
    ASSERT_TRUE (estimator.compute (views.data(), views.size()));

    const auto* signal = estimator.getChannelData (0);
    const auto* silent = estimator.getChannelData (1);
    EXPECT_GT (*std::max_element (signal, signal + estimator.getBinCount()), 0.0f);
    for (std::size_t bin = 0; bin < estimator.getBinCount(); ++bin)
        EXPECT_FLOAT_EQ (silent[bin], 0.0f);
}

TEST (MultitaperPeriodogramTests, WhiteNoiseHasExpectedOneSidedDensity)
{
    constexpr std::size_t sampleCount = 512;
    constexpr std::size_t channelCount = 64;
    constexpr double sampleRate = 30000.0;
    constexpr double sigma = 3.0;
    const auto bank = generateBank (sampleCount, 2.5, 4);
    ASSERT_TRUE (bank->succeeded());
    std::vector<float> samples (sampleCount * channelCount);
    std::mt19937 generator (13291);
    std::normal_distribution<float> noise (0.0f, static_cast<float> (sigma));
    for (auto& sample : samples)
        sample = noise (generator);

    std::vector<ChannelSampleView> views (channelCount);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
        views[channel] = { samples.data() + channel * sampleCount, sampleCount, nullptr, 0 };
    MultitaperPeriodogram estimator (
        channelCount, sampleRate, bank, DetrendMode::mean, fftwEstimate);
    ASSERT_TRUE (estimator.compute (views.data(), views.size()));

    double mean = 0.0;
    std::size_t count = 0;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto* psd = estimator.getChannelData (channel);
        for (std::size_t bin = 8; bin + 8 < estimator.getBinCount(); ++bin)
        {
            mean += psd[bin];
            ++count;
        }
    }
    mean /= static_cast<double> (count);
    const auto expected = 2.0 * sigma * sigma / sampleRate;
    EXPECT_NEAR (mean, expected, expected * 0.04);
}

TEST (MultitaperPeriodogramTests, RejectsBadConfiguration)
{
    const auto bank = generateBank (16, 2.0, 3);
    ASSERT_TRUE (bank->succeeded());
    EXPECT_THROW (
        MultitaperPeriodogram (0, 1000.0, bank, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);
    EXPECT_THROW (
        MultitaperPeriodogram (1, 0.0, bank, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);
    EXPECT_THROW (
        MultitaperPeriodogram (1, 1000.0, nullptr, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);

    auto malformed = std::make_shared<DpssTaperBank> (*bank);
    malformed->tapers.pop_back();
    EXPECT_THROW (
        MultitaperPeriodogram (1, 1000.0, malformed, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);

    malformed = std::make_shared<DpssTaperBank> (*bank);
    malformed->concentrationRatios[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW (
        MultitaperPeriodogram (1, 1000.0, malformed, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);

    malformed = std::make_shared<DpssTaperBank> (*bank);
    malformed->tapers[0] = std::numeric_limits<float>::infinity();
    EXPECT_THROW (
        MultitaperPeriodogram (1, 1000.0, malformed, DetrendMode::mean, fftwEstimate),
        std::invalid_argument);
}

TEST (MultitaperPeriodogramTests, RejectedInputPreservesLastValidOutput)
{
    constexpr std::size_t sampleCount = 64;
    auto samples = std::vector<float> (sampleCount, 1.0f);
    const auto bank = generateBank (sampleCount, 2.0, 3);
    ASSERT_TRUE (bank->succeeded());
    MultitaperPeriodogram estimator (
        1, 1000.0, bank, DetrendMode::none, fftwEstimate);
    ChannelSampleView view { samples.data(), sampleCount, nullptr, 0 };
    ASSERT_TRUE (estimator.compute (&view, 1));
    const std::vector<float> valid (
        estimator.getChannelData (0), estimator.getChannelData (0) + estimator.getBinCount());

    samples[17] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE (estimator.compute (&view, 1));
    EXPECT_EQ (std::vector<float> (
                   estimator.getChannelData (0),
                   estimator.getChannelData (0) + estimator.getBinCount()),
               valid);

    view = { samples.data(), sampleCount - 1, nullptr, 0 };
    EXPECT_FALSE (estimator.compute (&view, 1));
    EXPECT_EQ (estimator.getChannelData (1), nullptr);
}
} // namespace
