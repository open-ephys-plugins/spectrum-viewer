#include "gtest/gtest.h"

#include "SpectrumAmplitudeRange.h"

#include <limits>
#include <vector>

namespace
{
using spectrumviewer::AmplitudeRangeMode;
using spectrumviewer::SpectrumAmplitudeRange;

TEST (SpectrumAmplitudeRangeTests, AutomaticRangeIsDefault)
{
    SpectrumAmplitudeRange range;
    EXPECT_EQ (range.getMode(), AmplitudeRangeMode::automatic);
    EXPECT_FLOAT_EQ (range.getCurrentRange().minimum, -25.0f);
    EXPECT_FLOAT_EQ (range.getCurrentRange().maximum, 25.0f);
}

TEST (SpectrumAmplitudeRangeTests, FixedRangeNeverTracksFrames)
{
    SpectrumAmplitudeRange range;
    range.setMode (AmplitudeRangeMode::fixed);
    ASSERT_TRUE (range.setFixedRange (-90.0f, 10.0f));

    const std::vector<std::vector<float>> mean { { -40.0f, -30.0f } };
    const std::vector<std::vector<float>> peak { { -35.0f, 80.0f } };
    const auto updated = range.update (mean, peak, 1, 100.0);
    EXPECT_FLOAT_EQ (updated.minimum, -90.0f);
    EXPECT_FLOAT_EQ (updated.maximum, 10.0f);
}

TEST (SpectrumAmplitudeRangeTests, RejectsInvalidOrTooNarrowFixedRanges)
{
    SpectrumAmplitudeRange range;
    EXPECT_FALSE (range.setFixedRange (0.0f, 0.0f));
    EXPECT_FALSE (range.setFixedRange (-10.0f, 9.0f));
    EXPECT_FALSE (range.setFixedRange (
        std::numeric_limits<float>::quiet_NaN(), 20.0f));
    EXPECT_FLOAT_EQ (range.getFixedRange().minimum, -25.0f);
    EXPECT_FLOAT_EQ (range.getFixedRange().maximum, 25.0f);
}

TEST (SpectrumAmplitudeRangeTests, AutoFitRejectsLowOutlierAndIncludesPeakEnvelope)
{
    SpectrumAmplitudeRange range;
    range.setMode (AmplitudeRangeMode::automatic);
    EXPECT_FALSE (range.hasAutomaticRange());
    std::vector<float> means (100, -60.0f);
    means.front() = -200.0f;
    std::vector<float> peaks (100, -55.0f);
    peaks[50] = 12.0f;

    const auto fitted = range.update ({ means }, { peaks }, 1, 0.5);
    EXPECT_TRUE (range.hasAutomaticRange());
    EXPECT_NEAR (fitted.minimum, -63.0f, 1.0e-5f);
    EXPECT_NEAR (fitted.maximum, 15.0f, 1.0e-5f);
}

TEST (SpectrumAmplitudeRangeTests, AutoRangeUsesSlowSignalTimeIntegration)
{
    SpectrumAmplitudeRange range;
    range.setMode (AmplitudeRangeMode::automatic);
    const std::vector<std::vector<float>> initialMean { std::vector<float> (100, -60.0f) };
    const std::vector<std::vector<float>> initialPeak { std::vector<float> (100, -40.0f) };
    const auto initial = range.update (initialMean, initialPeak, 1, 0.5);

    const std::vector<std::vector<float>> changedMean { std::vector<float> (100, -80.0f) };
    const std::vector<std::vector<float>> changedPeak { std::vector<float> (100, 20.0f) };
    const auto unchanged = range.update (changedMean, changedPeak, 1, 0.0);
    EXPECT_FLOAT_EQ (unchanged.minimum, initial.minimum);
    EXPECT_FLOAT_EQ (unchanged.maximum, initial.maximum);

    const auto oneHop = range.update (changedMean, changedPeak, 1, 0.125);
    EXPECT_GT (oneHop.minimum, -83.0f);
    EXPECT_LT (oneHop.minimum, initial.minimum);
    EXPECT_GT (oneHop.maximum, initial.maximum);
    EXPECT_LT (oneHop.maximum, 23.0f);
}

TEST (SpectrumAmplitudeRangeTests, OutwardExpansionIsFasterThanInwardContraction)
{
    SpectrumAmplitudeRange outward;
    outward.setMode (AmplitudeRangeMode::automatic);
    outward.update ({ std::vector<float> (100, -60.0f) },
                     { std::vector<float> (100, -40.0f) }, 1, 1.0);
    const auto expanded = outward.update ({ std::vector<float> (100, -80.0f) },
                                           { std::vector<float> (100, 20.0f) }, 1, 2.0);

    SpectrumAmplitudeRange inward;
    inward.setMode (AmplitudeRangeMode::automatic);
    inward.update ({ std::vector<float> (100, -80.0f) },
                    { std::vector<float> (100, 20.0f) }, 1, 1.0);
    const auto contracted = inward.update ({ std::vector<float> (100, -60.0f) },
                                            { std::vector<float> (100, -40.0f) }, 1, 2.0);

    EXPECT_GT (expanded.maximum - (-37.0f),
               23.0f - contracted.maximum);
    EXPECT_GT (-63.0f - expanded.minimum,
               contracted.minimum - (-83.0f));
}

TEST (SpectrumAmplitudeRangeTests, UnitChangeCanReinitializeAutomaticFit)
{
    SpectrumAmplitudeRange range;
    range.setMode (AmplitudeRangeMode::automatic);
    range.update ({ std::vector<float> (100, -80.0f) },
                  { std::vector<float> (100, 20.0f) }, 1, 1.0);

    range.resetAutomatic();
    const auto reinitialized = range.update ({ std::vector<float> (100, -20.0f) },
                                              { std::vector<float> (100, 0.0f) }, 1, 0.125);
    EXPECT_FLOAT_EQ (reinitialized.minimum, -23.0f);
    EXPECT_FLOAT_EQ (reinitialized.maximum, 3.0f);
}
} // namespace
