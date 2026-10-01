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

#include "gtest/gtest.h"

#include "ReferencePeriodogram.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace
{
using spectrumviewer::DetrendMode;
using spectrumviewer::PeriodogramResult;
using spectrumviewer::ReferencePeriodogram;

constexpr double twoPi = 6.283185307179586476925286766559;

std::vector<double> rectangularTaper (std::size_t size)
{
    return std::vector<double> (size, 1.0);
}

std::vector<float> cosine (std::size_t size, double bin, double amplitude = 1.0)
{
    std::vector<float> samples (size);
    for (std::size_t sample = 0; sample < size; ++sample)
    {
        const auto phase = twoPi * bin * static_cast<double> (sample)
                           / static_cast<double> (size);
        samples[sample] = static_cast<float> (amplitude * std::cos (phase));
    }
    return samples;
}

double integratedPower (const PeriodogramResult& result, std::size_t channel = 0)
{
    const auto* psd = result.getChannelData (channel);
    double sum = 0.0;
    for (std::size_t bin = 0; bin < result.numBins; ++bin)
        sum += psd[bin];
    return sum * result.binWidth;
}

std::size_t peakBin (const PeriodogramResult& result, std::size_t channel)
{
    const auto* first = result.getChannelData (channel);
    return static_cast<std::size_t> (std::max_element (first, first + result.numBins) - first);
}

TEST (ReferencePeriodogramTests, RejectsInvalidInputs)
{
    const std::vector<float> samples (4, 1.0f);
    const auto taper = rectangularTaper (samples.size());

    EXPECT_THROW (ReferencePeriodogram::compute (nullptr, 1, 4, 4, 1000.0, taper, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 0, 4, 4, 1000.0, taper, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 3, 4, 1000.0, taper, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 4, 0, 1000.0, {}, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 4, 4, 0.0, taper, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 4, 4, 1000.0, { 1.0 }, DetrendMode::none), std::invalid_argument);
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 4, 4, 1000.0, std::vector<double> (4), DetrendMode::none), std::invalid_argument);

    auto nonfiniteTaper = taper;
    nonfiniteTaper[2] = std::numeric_limits<double>::infinity();
    EXPECT_THROW (ReferencePeriodogram::compute (samples.data(), 1, 4, 4, 1000.0, nonfiniteTaper, DetrendMode::none), std::invalid_argument);

    auto nonfiniteSamples = samples;
    nonfiniteSamples[1] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW (ReferencePeriodogram::compute (nonfiniteSamples.data(), 1, 4, 4, 1000.0, taper, DetrendMode::none), std::invalid_argument);
}

TEST (ReferencePeriodogramTests, CalibratesDcWithoutDoublingIt)
{
    constexpr std::size_t size = 16;
    constexpr double sampleRate = 1024.0;
    constexpr double amplitude = 3.0;
    const std::vector<float> samples (size, static_cast<float> (amplitude));
    const auto result = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, sampleRate, rectangularTaper (size), DetrendMode::none);

    EXPECT_NEAR (result.psd[0], amplitude * amplitude * static_cast<double> (size) / sampleRate, 1.0e-13);
    EXPECT_NEAR (integratedPower (result), amplitude * amplitude, 1.0e-12);
    for (std::size_t bin = 1; bin < result.numBins; ++bin)
        EXPECT_NEAR (result.psd[bin], 0.0, 1.0e-28);
}

TEST (ReferencePeriodogramTests, CalibratesEvenLengthNyquistWithoutDoublingIt)
{
    constexpr std::size_t size = 16;
    constexpr double sampleRate = 1024.0;
    constexpr double amplitude = 2.0;
    std::vector<float> samples (size);
    for (std::size_t sample = 0; sample < size; ++sample)
        samples[sample] = sample % 2 == 0 ? static_cast<float> (amplitude) : static_cast<float> (-amplitude);

    const auto result = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, sampleRate, rectangularTaper (size), DetrendMode::none);
    const auto nyquistBin = size / 2;

    EXPECT_NEAR (result.psd[nyquistBin], amplitude * amplitude * static_cast<double> (size) / sampleRate, 1.0e-13);
    EXPECT_NEAR (integratedPower (result), amplitude * amplitude, 1.0e-12);
}

TEST (ReferencePeriodogramTests, CalibratesABinCentredCosine)
{
    constexpr std::size_t size = 32;
    constexpr std::size_t toneBin = 3;
    constexpr double sampleRate = 2048.0;
    constexpr double amplitude = 2.0;
    const auto samples = cosine (size, static_cast<double> (toneBin), amplitude);
    const auto result = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, sampleRate, rectangularTaper (size), DetrendMode::none);

    const auto expectedDensity = amplitude * amplitude * static_cast<double> (size) / (2.0 * sampleRate);
    EXPECT_EQ (peakBin (result, 0), toneBin);
    EXPECT_NEAR (result.psd[toneBin], expectedDensity, 2.0e-9);
    EXPECT_NEAR (integratedPower (result), amplitude * amplitude / 2.0, 2.0e-7);
}

TEST (ReferencePeriodogramTests, ObeysParsevalForOddAndEvenWindows)
{
    for (const std::size_t size : { 15u, 16u })
    {
        std::vector<float> samples (size);
        std::vector<double> taper (size);
        double expectedNumerator = 0.0;
        double taperEnergy = 0.0;

        for (std::size_t sample = 0; sample < size; ++sample)
        {
            samples[sample] = static_cast<float> (0.25 * static_cast<double> (sample) - 1.0
                                                  + std::sin (0.7 * static_cast<double> (sample)));
            taper[sample] = 0.54 - 0.46 * std::cos (twoPi * static_cast<double> (sample) / static_cast<double> (size - 1));
            const auto windowed = static_cast<double> (samples[sample]) * taper[sample];
            expectedNumerator += windowed * windowed;
            taperEnergy += taper[sample] * taper[sample];
        }

        const auto result = ReferencePeriodogram::compute (
            samples.data(), 1, size, size, 30000.0, taper, DetrendMode::none);
        EXPECT_NEAR (integratedPower (result), expectedNumerator / taperEnergy, 1.0e-11);
    }
}

TEST (ReferencePeriodogramTests, OffBinToneSpreadsEnergyButPreservesIntegratedPower)
{
    constexpr std::size_t size = 31;
    const auto samples = cosine (size, 4.25, 1.5);
    const auto result = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, 1000.0, rectangularTaper (size), DetrendMode::none);

    double expectedPower = 0.0;
    for (const auto sample : samples)
        expectedPower += static_cast<double> (sample) * static_cast<double> (sample);
    expectedPower /= static_cast<double> (size);

    const auto peak = *std::max_element (result.psd.begin(), result.psd.end());
    const auto significantBins = static_cast<std::size_t> (std::count_if (
        result.psd.begin(), result.psd.end(), [peak] (double value)
        { return value > peak * 1.0e-3; }));
    EXPECT_GT (significantBins, 1u);
    EXPECT_NEAR (integratedPower (result), expectedPower, 1.0e-11);
}

TEST (ReferencePeriodogramTests, MeanDetrendingRemovesDcAndIsOffsetInvariant)
{
    const std::vector<float> samples { -4.0f, -1.0f, 3.0f, 2.0f, 7.0f, -5.0f, 1.0f, -3.0f };
    auto offsetSamples = samples;
    for (auto& sample : offsetSamples)
        sample += 128.0f;
    const auto taper = rectangularTaper (samples.size());
    const auto original = ReferencePeriodogram::compute (
        samples.data(), 1, samples.size(), samples.size(), 1000.0, taper, DetrendMode::mean);
    const auto offset = ReferencePeriodogram::compute (
        offsetSamples.data(), 1, samples.size(), samples.size(), 1000.0, taper, DetrendMode::mean);

    EXPECT_NEAR (original.psd[0], 0.0, 1.0e-29);
    ASSERT_EQ (original.psd.size(), offset.psd.size());
    for (std::size_t bin = 0; bin < original.psd.size(); ++bin)
        EXPECT_NEAR (original.psd[bin], offset.psd[bin], 1.0e-13);
}

TEST (ReferencePeriodogramTests, LinearDetrendingRemovesAnAffineSignal)
{
    constexpr std::size_t size = 17;
    std::vector<float> samples (size);
    for (std::size_t sample = 0; sample < size; ++sample)
        samples[sample] = 4.0f + 0.5f * static_cast<float> (sample);
    const auto taper = rectangularTaper (size);
    const auto meanOnly = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, 1000.0, taper, DetrendMode::mean);
    const auto linear = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, 1000.0, taper, DetrendMode::linear);

    EXPECT_GT (integratedPower (meanOnly), 1.0);
    EXPECT_NEAR (integratedPower (linear), 0.0, 1.0e-27);
}

TEST (ReferencePeriodogramTests, ReportsCorrectOddAndEvenFrequencyAxes)
{
    for (const std::size_t size : { 15u, 16u })
    {
        const std::vector<float> samples (size, 0.0f);
        const auto result = ReferencePeriodogram::compute (
            samples.data(), 1, size, size, 1200.0, rectangularTaper (size), DetrendMode::none);

        EXPECT_EQ (result.numBins, size / 2 + 1);
        EXPECT_DOUBLE_EQ (result.binWidth, 1200.0 / static_cast<double> (size));
        EXPECT_DOUBLE_EQ (result.getFrequency (result.numBins - 1),
                          static_cast<double> (size / 2) * result.binWidth);
        if (size % 2 == 0)
            EXPECT_DOUBLE_EQ (result.getFrequency (result.numBins - 1), 600.0);
        else
            EXPECT_LT (result.getFrequency (result.numBins - 1), 600.0);
    }
}

TEST (ReferencePeriodogramTests, KeepsPlanarChannelsIsolatedAndIgnoresStridePadding)
{
    constexpr std::size_t size = 32;
    constexpr std::size_t stride = size + 3;
    std::vector<float> planar (2 * stride, 10000.0f);
    const auto first = cosine (size, 2.0);
    const auto second = cosine (size, 5.0);
    std::copy (first.begin(), first.end(), planar.begin());
    std::copy (second.begin(), second.end(), planar.begin() + static_cast<std::ptrdiff_t> (stride));

    const auto result = ReferencePeriodogram::compute (
        planar.data(), 2, stride, size, 1000.0, rectangularTaper (size), DetrendMode::none);

    EXPECT_EQ (peakBin (result, 0), 2u);
    EXPECT_EQ (peakBin (result, 1), 5u);
    EXPECT_GT (result.getChannelData (0)[2], result.getChannelData (1)[2] * 1.0e10);
    EXPECT_GT (result.getChannelData (1)[5], result.getChannelData (0)[5] * 1.0e10);
}

TEST (ReferencePeriodogramTests, WhiteNoiseHasTheExpectedOneSidedDensity)
{
    constexpr std::size_t size = 64;
    constexpr std::size_t realizations = 256;
    constexpr double sampleRate = 2000.0;
    constexpr double expectedInteriorDensity = 2.0 / sampleRate;
    std::vector<double> meanPsd (size / 2 + 1, 0.0);
    std::vector<float> samples (size);
    std::mt19937 generator (73421);
    std::normal_distribution<float> normal (0.0f, 1.0f);

    for (std::size_t realization = 0; realization < realizations; ++realization)
    {
        for (auto& sample : samples)
            sample = normal (generator);
        const auto result = ReferencePeriodogram::compute (
            samples.data(), 1, size, size, sampleRate, rectangularTaper (size), DetrendMode::none);
        for (std::size_t bin = 0; bin < result.numBins; ++bin)
            meanPsd[bin] += result.psd[bin] / static_cast<double> (realizations);
    }

    for (std::size_t bin = 1; bin < size / 2; ++bin)
        EXPECT_NEAR (meanPsd[bin], expectedInteriorDensity, expectedInteriorDensity * 0.25);
}
} // namespace
