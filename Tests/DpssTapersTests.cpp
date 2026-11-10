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

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
double dot (const float* left, const float* right, std::size_t count)
{
    double value = 0.0;
    for (std::size_t sample = 0; sample < count; ++sample)
        value += static_cast<double> (left[sample]) * static_cast<double> (right[sample]);
    return value;
}
} // namespace

TEST (DpssTapersTests, RejectsInvalidConfigurations)
{
    using spectrumviewer::DpssGenerationStatus;
    using spectrumviewer::generateDpssTapers;

    EXPECT_EQ (generateDpssTapers (1, 1.0, 1).status,
               DpssGenerationStatus::invalidSampleCount);
    EXPECT_EQ (generateDpssTapers (16, 0.0, 1).status,
               DpssGenerationStatus::invalidTimeHalfBandwidth);
    EXPECT_EQ (generateDpssTapers (16, std::numeric_limits<double>::infinity(), 1).status,
               DpssGenerationStatus::invalidTimeHalfBandwidth);
    EXPECT_EQ (generateDpssTapers (16, 8.0, 1).status,
               DpssGenerationStatus::invalidTimeHalfBandwidth);
    EXPECT_EQ (generateDpssTapers (16, 2.0, 0).status,
               DpssGenerationStatus::invalidTaperCount);
    EXPECT_EQ (generateDpssTapers (16, 2.0, 17).status,
               DpssGenerationStatus::invalidTaperCount);
}

TEST (DpssTapersTests, MatchesSmallSciPyGoldenValues)
{
    const auto bank = spectrumviewer::generateDpssTapers (4, 1.0, 2);
    ASSERT_TRUE (bank.succeeded())
        << spectrumviewer::getDpssGenerationStatusDescription (bank.status);

    constexpr double expectedTapers[][4] {
        { 0.3336539389003113, 0.6234380875887414, 0.6234380875887414, 0.3336539389003113 },
        { 0.6234380875887414, 0.33365393890031136, -0.33365393890031136, -0.6234380875887414 }
    };
    constexpr double expectedRatios[] { 0.9886641674355178, 0.7764575766463244 };

    for (std::size_t taper = 0; taper < 2; ++taper)
    {
        for (std::size_t sample = 0; sample < 4; ++sample)
            EXPECT_NEAR (bank.getTaper (taper)[sample], expectedTapers[taper][sample], 3.0e-7);
        EXPECT_NEAR (bank.concentrationRatios[taper], expectedRatios[taper], 2.0e-13);
    }
}

TEST (DpssTapersTests, ProducesOrthonormalSymmetricTapers)
{
    constexpr std::size_t sampleCount = 64;
    constexpr std::size_t taperCount = 5;
    const auto bank = spectrumviewer::generateDpssTapers (sampleCount, 3.0, taperCount);
    ASSERT_TRUE (bank.succeeded());

    constexpr double expectedRatios[] {
        0.9999998730411327,
        0.9999911515902709,
        0.999723369225552,
        0.9950049123408422,
        0.946584183645627
    };
    for (std::size_t taper = 0; taper < taperCount; ++taper)
    {
        const auto* values = bank.getTaper (taper);
        EXPECT_NEAR (dot (values, values, sampleCount), 1.0, 2.0e-7);
        EXPECT_NEAR (bank.concentrationRatios[taper], expectedRatios[taper], 3.0e-12);
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            const auto expectedSign = taper % 2 == 0 ? 1.0f : -1.0f;
            EXPECT_NEAR (values[sample],
                         expectedSign * values[sampleCount - 1 - sample],
                         2.0e-7);
        }
        for (std::size_t other = 0; other < taper; ++other)
            EXPECT_NEAR (dot (values, bank.getTaper (other), sampleCount), 0.0, 2.0e-7);
    }
}

TEST (DpssTapersTests, ReportsTransitionTaperConcentration)
{
    const auto bank = spectrumviewer::generateDpssTapers (16, 2.0, 3);
    ASSERT_TRUE (bank.succeeded());
    constexpr double expectedRatios[] {
        0.9999565558039708,
        0.9979316293703271,
        0.9623464298366198
    };
    for (std::size_t taper = 0; taper < 3; ++taper)
        EXPECT_NEAR (bank.concentrationRatios[taper], expectedRatios[taper], 3.0e-12);
}

TEST (DpssTapersTests, HandlesProductionScaleBank)
{
    const auto bank = spectrumviewer::generateDpssTapers (60000, 3.0, 5);
    ASSERT_TRUE (bank.succeeded());
    constexpr double expectedRatios[] {
        0.9999998651882237,
        0.9999907545678598,
        0.9997149848670476,
        0.9949143842960144,
        0.94613788963302
    };
    for (std::size_t taper = 0; taper < 5; ++taper)
        EXPECT_NEAR (bank.concentrationRatios[taper], expectedRatios[taper], 2.0e-11);
}
