#include "AperiodicSpectrumBaseline.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace spectrumviewer
{
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
} // namespace spectrumviewer
