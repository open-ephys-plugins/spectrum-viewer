#include "gtest/gtest.h"

#include "SpectrumReference.h"

#include <cmath>
#include <limits>
#include <vector>

namespace
{
spectrumviewer::SpectrumFrameDescriptor makeDescriptor()
{
    spectrumviewer::SpectrumFrameDescriptor descriptor;
    descriptor.sampleRateHz = 80.0;
    descriptor.windowSampleCount = 160;
    descriptor.hopSampleCount = 160;
    descriptor.binWidthHz = 0.5;
    descriptor.timeHalfBandwidth = 3.0;
    descriptor.taperCount = 5;
    descriptor.detrendMode = spectrumviewer::DetrendMode::mean;
    return descriptor;
}

spectrumviewer::CapturedSpectrum makeCapture()
{
    spectrumviewer::SpectrumCaptureQuality quality;
    quality.includedWindowCount = 2;
    quality.targetWindowCount = 2;
    quality.firstSample = 100;
    quality.lastSampleExclusive = 420;
    return { 7,
             123456789,
             makeDescriptor(),
             { 2 },
             { "uV" },
             std::vector<float> (81, 4.0f),
             std::vector<float> (81, 0.25f),
             quality };
}

TEST (SpectrumReferenceTests, RetainsFullResolutionCaptureAndQuality)
{
    const auto capture = makeCapture();
    EXPECT_EQ (capture.getCaptureId(), 7u);
    EXPECT_EQ (capture.getCapturedAtUnixMilliseconds(), 123456789);
    EXPECT_EQ (capture.getChannelCount(), 1u);
    EXPECT_EQ (capture.getBinCount(), 81u);
    EXPECT_FLOAT_EQ (capture.getPlanarMeanPsd()[40], 4.0f);
    EXPECT_FLOAT_EQ (capture.getPlanarSampleVariance()[40], 0.25f);
    EXPECT_EQ (capture.getQuality().firstSample, 100);
    EXPECT_EQ (capture.getQuality().lastSampleExclusive, 420);
}

TEST (SpectrumReferenceTests, CompatibilityIgnoresOnlySchedulingMetadata)
{
    const auto capture = makeCapture();
    auto candidate = makeDescriptor();
    candidate.configurationGeneration = 99;
    candidate.hopSampleCount = 40;
    EXPECT_TRUE (capture.isCompatibleWith (candidate, { 2 }, { "uV" }));

    candidate.taperCount = 4;
    EXPECT_FALSE (capture.isCompatibleWith (candidate, { 2 }, { "uV" }));
    candidate = makeDescriptor();
    EXPECT_FALSE (capture.isCompatibleWith (candidate, { 3 }, { "uV" }));
    EXPECT_FALSE (capture.isCompatibleWith (candidate, { 2 }, { "mV" }));
}

TEST (SpectrumReferenceTests, RejectsIncompleteOrInvalidSnapshots)
{
    auto quality = spectrumviewer::SpectrumCaptureQuality {};
    quality.includedWindowCount = 1;
    quality.targetWindowCount = 2;
    quality.lastSampleExclusive = 160;
    EXPECT_THROW ((spectrumviewer::CapturedSpectrum { 1,
                                                     123456789,
                                                     makeDescriptor(),
                                                     { 0 },
                                                     { "uV" },
                                                     std::vector<float> (81, 1.0f),
                                                     std::vector<float> (81, 0.0f),
                                                     quality }),
                  std::invalid_argument);
}

TEST (SpectrumReferenceTests, DecibelDeltaHandlesKnownRatiosAndInvalidBins)
{
    const float current[] { 1.0f, 2.0f, 4.0f, 0.0f,
                            std::numeric_limits<float>::infinity() };
    const float reference[] { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    float delta[5] {};
    ASSERT_TRUE (spectrumviewer::computeDecibelDelta (
        current, reference, delta, 5));
    EXPECT_FLOAT_EQ (delta[0], 0.0f);
    EXPECT_NEAR (delta[1], 10.0 * std::log10 (2.0), 1.0e-5);
    EXPECT_NEAR (delta[2], 10.0 * std::log10 (4.0), 1.0e-5);
    EXPECT_TRUE (std::isnan (delta[3]));
    EXPECT_TRUE (std::isnan (delta[4]));
}
} // namespace
