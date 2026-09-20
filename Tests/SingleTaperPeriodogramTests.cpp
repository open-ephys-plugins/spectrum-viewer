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
#include "SingleTaperPeriodogram.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

namespace
{
using spectrumviewer::ChannelSampleView;
using spectrumviewer::DetrendMode;
using spectrumviewer::ReferencePeriodogram;
using spectrumviewer::SingleTaperPeriodogram;

constexpr double twoPi = 6.283185307179586476925286766559;

std::vector<float> makeTaper (std::size_t size)
{
    std::vector<float> taper (size);
    for (std::size_t sample = 0; sample < size; ++sample)
    {
        taper[sample] = static_cast<float> (
            0.54 - 0.46 * std::cos (twoPi * static_cast<double> (sample) / static_cast<double> (size - 1)));
    }
    return taper;
}

std::vector<double> asDouble (const std::vector<float>& values)
{
    return { values.begin(), values.end() };
}

void expectMatchesReference (std::size_t size, DetrendMode mode)
{
    constexpr std::size_t channels = 3;
    const auto stride = size + 5;
    std::vector<float> planar (channels * stride, -9999.0f);
    std::mt19937 generator (static_cast<std::uint32_t> (9182 + size));
    std::normal_distribution<float> noise (0.0f, 0.2f);

    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        auto* destination = planar.data() + channel * stride;
        for (std::size_t sample = 0; sample < size; ++sample)
        {
            destination[sample] = static_cast<float> (
                                      20.0 + 0.03 * static_cast<double> (sample)
                                      + (1.0 + static_cast<double> (channel))
                                            * std::cos (twoPi * static_cast<double> ((channel + 1) * 3 * sample)
                                                        / static_cast<double> (size)))
                                  + noise (generator);
        }
    }

    const auto taper = makeTaper (size);
    const auto expected = ReferencePeriodogram::compute (
        planar.data(), channels, stride, size, 30000.0, asDouble (taper), mode);
    SingleTaperPeriodogram estimator (channels, size, 30000.0, taper, mode);
    std::array<ChannelSampleView, channels> views;
    for (std::size_t channel = 0; channel < channels; ++channel)
        views[channel] = { planar.data() + channel * stride, size, nullptr, 0 };

    ASSERT_TRUE (estimator.compute (views.data(), views.size()));
    ASSERT_EQ (estimator.getBinCount(), expected.numBins);
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        const auto* actual = estimator.getChannelData (channel);
        const auto* reference = expected.getChannelData (channel);
        for (std::size_t bin = 0; bin < expected.numBins; ++bin)
        {
            const auto tolerance = std::max (1.0e-10, std::abs (reference[bin]) * 2.0e-4);
            EXPECT_NEAR (static_cast<double> (actual[bin]), reference[bin], tolerance)
                << "channel=" << channel << " bin=" << bin << " N=" << size;
        }
    }
}

TEST (SingleTaperPeriodogramTests, MatchesDoubleOracleAcrossLengthsAndDetrendingModes)
{
    for (const auto size : { 15u, 16u, 31u, 64u })
        for (const auto mode : { DetrendMode::none, DetrendMode::mean, DetrendMode::linear })
            expectMatchesReference (size, mode);
}

TEST (SingleTaperPeriodogramTests, SplitAndContiguousViewsProduceTheSameSpectrum)
{
    constexpr std::size_t size = 127;
    constexpr std::size_t split = 43;
    std::vector<float> contiguous (size);
    for (std::size_t sample = 0; sample < size; ++sample)
        contiguous[sample] = static_cast<float> (2.0 + 0.01 * static_cast<double> (sample)
                                                 + std::cos (twoPi * 11.3 * static_cast<double> (sample)
                                                             / static_cast<double> (size)));
    std::vector<float> circular (size);
    std::copy (contiguous.begin(), contiguous.end() - static_cast<std::ptrdiff_t> (split), circular.begin() + split);
    std::copy (contiguous.end() - static_cast<std::ptrdiff_t> (split), contiguous.end(), circular.begin());

    const auto taper = makeTaper (size);
    SingleTaperPeriodogram contiguousEstimator (1, size, 2000.0, taper, DetrendMode::linear);
    SingleTaperPeriodogram splitEstimator (1, size, 2000.0, taper, DetrendMode::linear);
    const ChannelSampleView contiguousView { contiguous.data(), size, nullptr, 0 };
    const ChannelSampleView splitView { circular.data() + split, size - split, circular.data(), split };
    ASSERT_TRUE (contiguousEstimator.compute (&contiguousView, 1));
    ASSERT_TRUE (splitEstimator.compute (&splitView, 1));

    for (std::size_t bin = 0; bin < contiguousEstimator.getBinCount(); ++bin)
        EXPECT_FLOAT_EQ (contiguousEstimator.getChannelData (0)[bin], splitEstimator.getChannelData (0)[bin]);
}

TEST (SingleTaperPeriodogramTests, RetainsAWeakToneBesideAStrongToneAfterLargeOffsetRemoval)
{
    constexpr std::size_t size = 256;
    constexpr std::size_t strongBin = 17;
    constexpr std::size_t weakBin = 41;
    std::vector<float> samples (size);
    for (std::size_t sample = 0; sample < size; ++sample)
    {
        const auto phase = twoPi * static_cast<double> (sample) / static_cast<double> (size);
        samples[sample] = static_cast<float> (10000.0 + 3.0 * std::cos (strongBin * phase)
                                              + 0.02 * std::cos (weakBin * phase));
    }
    const std::vector<float> taper (size, 1.0f);
    const auto expected = ReferencePeriodogram::compute (
        samples.data(), 1, size, size, 30000.0, asDouble (taper), DetrendMode::mean);
    SingleTaperPeriodogram estimator (1, size, 30000.0, taper, DetrendMode::mean);
    const ChannelSampleView view { samples.data(), size, nullptr, 0 };
    ASSERT_TRUE (estimator.compute (&view, 1));

    EXPECT_NEAR (estimator.getChannelData (0)[strongBin], expected.psd[strongBin], expected.psd[strongBin] * 2.0e-4);
    EXPECT_NEAR (estimator.getChannelData (0)[weakBin], expected.psd[weakBin], expected.psd[weakBin] * 0.03);
    EXPECT_GT (estimator.getChannelData (0)[weakBin], estimator.getChannelData (0)[weakBin + 3] * 100.0f);
}

TEST (SingleTaperPeriodogramTests, RejectsMalformedAndNonFiniteViewsWithoutAllocating)
{
    constexpr std::size_t size = 16;
    const std::vector<float> taper (size, 1.0f);
    SingleTaperPeriodogram estimator (1, size, 1000.0, taper, DetrendMode::mean);
    std::vector<float> samples (size, 1.0f);
    ChannelSampleView view { samples.data(), size - 1, nullptr, 0 };

    EXPECT_FALSE (estimator.compute (nullptr, 1));
    EXPECT_FALSE (estimator.compute (&view, 1));
    view = { samples.data(), size, nullptr, 0 };
    samples[4] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE (estimator.compute (&view, 1));
}
} // namespace
