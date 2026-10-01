#include "gtest/gtest.h"

#include "SpectrumCaptureAccumulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::SpectrumCaptureAccumulator;

TEST (SpectrumCaptureAccumulatorTests, RejectsInvalidConfiguration)
{
    EXPECT_THROW ((SpectrumCaptureAccumulator { 0, 2, 1 }), std::invalid_argument);
    EXPECT_THROW ((SpectrumCaptureAccumulator { 1, 0, 1 }), std::invalid_argument);
    EXPECT_THROW ((SpectrumCaptureAccumulator { 1, 2, 0 }), std::invalid_argument);
}

TEST (SpectrumCaptureAccumulatorTests, ComputesPlanarMeanAndSampleVariance)
{
    SpectrumCaptureAccumulator capture (2, 2, 3);
    ASSERT_TRUE (capture.add (std::vector<float> { 1.0f, 3.0f, 10.0f, 14.0f }.data(), 2, 2));
    ASSERT_TRUE (capture.add (std::vector<float> { 2.0f, 5.0f, 14.0f, 18.0f }.data(), 2, 2));
    ASSERT_TRUE (capture.add (std::vector<float> { 3.0f, 7.0f, 18.0f, 22.0f }.data(), 2, 2));

    EXPECT_TRUE (capture.isComplete());
    EXPECT_EQ (capture.getIncludedWindowCount(), 3u);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[0], 2.0f);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[1], 5.0f);
    EXPECT_FLOAT_EQ (capture.getChannelMean (1)[0], 14.0f);
    EXPECT_FLOAT_EQ (capture.getChannelMean (1)[1], 18.0f);
    EXPECT_FLOAT_EQ (capture.getSampleVariance (0, 0), 1.0f);
    EXPECT_FLOAT_EQ (capture.getSampleVariance (0, 1), 4.0f);
    EXPECT_FLOAT_EQ (capture.getSampleVariance (1, 0), 16.0f);
    EXPECT_FLOAT_EQ (capture.getSampleVariance (1, 1), 16.0f);
}

TEST (SpectrumCaptureAccumulatorTests, RejectsACompleteInvalidFrameWithoutMutation)
{
    SpectrumCaptureAccumulator capture (1, 3, 2);
    const std::vector<float> first { 2.0f, 4.0f, 6.0f };
    ASSERT_TRUE (capture.add (first.data(), 1, 3));
    const std::vector<float> invalid {
        10.0f, std::numeric_limits<float>::quiet_NaN(), 12.0f
    };
    EXPECT_FALSE (capture.add (invalid.data(), 1, 3));
    EXPECT_EQ (capture.getIncludedWindowCount(), 1u);
    EXPECT_EQ (capture.getRejectedWindowCount(), 1u);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[0], 2.0f);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[1], 4.0f);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[2], 6.0f);
}

TEST (SpectrumCaptureAccumulatorTests, RejectsShapeChangesAndFramesAfterCompletion)
{
    SpectrumCaptureAccumulator capture (1, 2, 1);
    const std::vector<float> values { 1.0f, 2.0f };
    EXPECT_FALSE (capture.add (values.data(), 2, 1));
    ASSERT_TRUE (capture.add (values.data(), 1, 2));
    EXPECT_FALSE (capture.add (values.data(), 1, 2));
    EXPECT_EQ (capture.getIncludedWindowCount(), 1u);
    EXPECT_EQ (capture.getRejectedWindowCount(), 2u);
}

TEST (SpectrumCaptureAccumulatorTests, ResetReusesStorage)
{
    SpectrumCaptureAccumulator capture (1, 1, 2);
    const float value = 9.0f;
    ASSERT_TRUE (capture.add (&value, 1, 1));
    const auto* storage = capture.getPlanarMean();
    capture.reset();
    EXPECT_EQ (capture.getPlanarMean(), storage);
    EXPECT_EQ (capture.getIncludedWindowCount(), 0u);
    EXPECT_EQ (capture.getRejectedWindowCount(), 0u);
    EXPECT_FLOAT_EQ (capture.getChannelMean (0)[0], 0.0f);
}

TEST (SpectrumCaptureAccumulatorTests, FloatAccumulationMatchesDoubleOracle)
{
    constexpr std::size_t binCount = 257;
    constexpr std::size_t windowCount = 30;
    SpectrumCaptureAccumulator capture (2, binCount, windowCount);
    std::vector<float> frame (2 * binCount);
    std::vector<double> oracleMean (frame.size(), 0.0);
    std::vector<double> oracleM2 (frame.size(), 0.0);
    std::mt19937 generator (0x5eedu);
    std::uniform_real_distribution<float> distribution (1.0e-7f, 1.0e3f);

    for (std::size_t window = 0; window < windowCount; ++window)
    {
        for (std::size_t index = 0; index < frame.size(); ++index)
        {
            frame[index] = distribution (generator);
            const auto value = static_cast<double> (frame[index]);
            const auto delta = value - oracleMean[index];
            oracleMean[index] += delta / static_cast<double> (window + 1);
            oracleM2[index] += delta * (value - oracleMean[index]);
        }
        ASSERT_TRUE (capture.add (frame.data(), 2, binCount));
    }

    for (std::size_t index = 0; index < frame.size(); ++index)
    {
        const auto expectedMean = oracleMean[index];
        const auto expectedVariance = oracleM2[index]
                                      / static_cast<double> (windowCount - 1);
        const auto channel = index / binCount;
        const auto bin = index % binCount;
        EXPECT_NEAR (capture.getChannelMean (channel)[bin], expectedMean,
                     std::max (1.0e-6, std::abs (expectedMean) * 5.0e-7));
        EXPECT_NEAR (capture.getSampleVariance (channel, bin), expectedVariance,
                     std::max (1.0e-5, std::abs (expectedVariance) * 2.0e-6));
    }
}
} // namespace
