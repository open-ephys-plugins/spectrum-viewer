#include "AperiodicSpectrumBaseline.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace spectrumviewer
{
namespace
{
    constexpr std::size_t powerLawWindowSamples = 4096;
    constexpr std::size_t powerLawBinCount = powerLawWindowSamples / 2 + 1;
    constexpr double powerLawSampleRateHz = 2000.0;

    std::vector<float> makePowerLawPsd()
    {
        const auto binWidth = powerLawSampleRateHz / powerLawWindowSamples;
        std::vector<float> psd (powerLawBinCount);
        for (std::size_t bin = 0; bin < powerLawBinCount; ++bin)
        {
            const auto frequency = std::max (binWidth,
                                             static_cast<double> (bin) * binWidth);
            psd[bin] = static_cast<float> (1.0e-4 / frequency + 1.0e-9);
        }
        return psd;
    }
} // namespace

TEST (AperiodicSpectrumBaselineTests, PreservesNarrowLineAbovePowerLawBackground)
{
    constexpr std::size_t windowSamples = 4096;
    constexpr std::size_t binCount = windowSamples / 2 + 1;
    constexpr double sampleRateHz = 2000.0;
    const auto binWidth = sampleRateHz / windowSamples;
    std::vector<float> psd (binCount);
    for (std::size_t bin = 0; bin < binCount; ++bin)
    {
        const auto frequency = std::max (binWidth,
                                         static_cast<double> (bin) * binWidth);
        psd[bin] = static_cast<float> (1.0e-4 / frequency + 1.0e-9);
    }

    const auto lineBin = static_cast<std::size_t> (std::round (300.0 / binWidth));
    psd[lineBin] *= 1000.0f;
    const float outputFrequencies[] { 20.0f, 50.0f, 100.0f, 300.0f, 900.0f };
    float baselineDb[5] {};
    AperiodicSpectrumBaseline estimator (binCount, 5);

    ASSERT_TRUE (estimator.estimate (psd.data(), 1, binCount, sampleRateHz,
                                     windowSamples, outputFrequencies, 5,
                                     baselineDb));
    for (std::size_t index = 0; index < 5; ++index)
    {
        const auto expected = 10.0f * std::log10 (
            static_cast<float> (1.0e-4 / outputFrequencies[index] + 1.0e-9));
        EXPECT_NEAR (baselineDb[index], expected, 1.5f);
    }

    const auto rawLineDb = 10.0f * std::log10 (psd[lineBin]);
    EXPECT_GT (rawLineDb - baselineDb[3], 25.0f);
}

TEST (AperiodicSpectrumBaselineTests, RejectsInvalidConfiguration)
{
    AperiodicSpectrumBaseline estimator (8, 4);
    float psd[8] {};
    float frequencies[4] {};
    float baseline[4] {};
    EXPECT_FALSE (estimator.estimate (psd, 1, 8, 1000.0, 16,
                                      frequencies, 4, baseline));
}

TEST (AperiodicSpectrumBaselineTests, OutputOrderDoesNotChangeTheFit)
{
    const auto psd = makePowerLawPsd();
    const float ascending[] { 20.0f, 50.0f, 100.0f, 300.0f, 900.0f };
    const float shuffled[] { 300.0f, 20.0f, 900.0f, 50.0f, 100.0f };
    float ascendingDb[5] {};
    float shuffledDb[5] {};
    AperiodicSpectrumBaseline estimator (powerLawBinCount, 5);

    ASSERT_TRUE (estimator.estimate (psd.data(), 1, powerLawBinCount, powerLawSampleRateHz,
                                     powerLawWindowSamples, ascending, 5, ascendingDb));
    ASSERT_TRUE (estimator.estimate (psd.data(), 1, powerLawBinCount, powerLawSampleRateHz,
                                     powerLawWindowSamples, shuffled, 5, shuffledDb));

    EXPECT_FLOAT_EQ (shuffledDb[0], ascendingDb[3]);
    EXPECT_FLOAT_EQ (shuffledDb[1], ascendingDb[0]);
    EXPECT_FLOAT_EQ (shuffledDb[2], ascendingDb[4]);
    EXPECT_FLOAT_EQ (shuffledDb[3], ascendingDb[1]);
    EXPECT_FLOAT_EQ (shuffledDb[4], ascendingDb[2]);
}

TEST (AperiodicSpectrumBaselineTests, UnfittableChannelDoesNotRemoveTheOthersFit)
{
    // A disconnected channel reads as zero power everywhere. That channel has
    // no background, but the live channel beside it still does.
    const auto live = makePowerLawPsd();
    std::vector<float> planar (live);
    planar.insert (planar.end(), powerLawBinCount, 0.0f);
    const float frequencies[] { 20.0f, 100.0f, 900.0f };
    float singleDb[3] {};
    float planarDb[6] {};
    AperiodicSpectrumBaseline estimator (powerLawBinCount, 3);

    ASSERT_TRUE (estimator.estimate (live.data(), 1, powerLawBinCount, powerLawSampleRateHz,
                                     powerLawWindowSamples, frequencies, 3, singleDb));
    ASSERT_TRUE (estimator.estimate (planar.data(), 2, powerLawBinCount, powerLawSampleRateHz,
                                     powerLawWindowSamples, frequencies, 3, planarDb));

    for (std::size_t index = 0; index < 3; ++index)
    {
        EXPECT_FLOAT_EQ (planarDb[index], singleDb[index]);
        EXPECT_TRUE (std::isnan (planarDb[3 + index]));
    }
}
} // namespace spectrumviewer
