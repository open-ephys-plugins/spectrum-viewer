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

#include <OpenEphysFFTWBatch.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
template <typename Sample>
using Batch = std::conditional_t<std::is_same_v<Sample, float>,
                                 FFTWRealToComplexBatchFloat,
                                 FFTWRealToComplexBatchDouble>;

template <typename Sample>
void verifyBatch()
{
    constexpr int size = 8;
    constexpr int count = 2;
    Batch<Sample> batch (size, count);

    EXPECT_EQ (batch.getTransformLength(), size);
    EXPECT_EQ (batch.getTransformCount(), count);
    EXPECT_EQ (batch.getBinCount(), size / 2 + 1);
    EXPECT_EQ (batch.getInputPointer (1) - batch.getInputPointer (0), size);
    EXPECT_EQ (batch.getOutputPointer (1) - batch.getOutputPointer (0), size / 2 + 1);
    EXPECT_EQ (batch.getInputPointer (-1), nullptr);
    EXPECT_EQ (batch.getOutputPointer (count), nullptr);

    for (int sample = 0; sample < size; ++sample)
    {
        batch.getInputPointer (0)[sample] = Sample { 1 };
        batch.getInputPointer (1)[sample] = sample % 2 == 0 ? Sample { 1 } : Sample { -1 };
    }
    batch.execute();

    const auto tolerance = std::is_same_v<Sample, float> ? 1.0e-5 : 1.0e-12;
    EXPECT_NEAR (std::abs (batch.getOutputPointer (0)[0]), static_cast<double> (size), tolerance);
    EXPECT_NEAR (std::abs (batch.getOutputPointer (1)[size / 2]), static_cast<double> (size), tolerance);
    for (int bin = 1; bin < batch.getBinCount(); ++bin)
        EXPECT_NEAR (std::abs (batch.getOutputPointer (0)[bin]), 0.0, tolerance);
    for (int bin = 0; bin < batch.getBinCount() - 1; ++bin)
        EXPECT_NEAR (std::abs (batch.getOutputPointer (1)[bin]), 0.0, tolerance);
}

TEST (OpenEphysFFTWBatchTests, ExecutesIndependentFloatTransforms)
{
    verifyBatch<float>();
}

TEST (OpenEphysFFTWBatchTests, ExecutesIndependentDoubleTransforms)
{
    verifyBatch<double>();
}

TEST (OpenEphysFFTWBatchTests, RejectsInvalidDimensions)
{
    EXPECT_THROW ((FFTWRealToComplexBatchFloat { 0, 1 }), std::invalid_argument);
    EXPECT_THROW ((FFTWRealToComplexBatchFloat { 8, 0 }), std::invalid_argument);
    EXPECT_THROW ((FFTWRealToComplexBatchDouble { -1, 1 }), std::invalid_argument);
}

TEST (OpenEphysFFTWBatchTests, SerializesConcurrentPlanConstruction)
{
    std::atomic<bool> valid { true };
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread)
    {
        threads.emplace_back ([&valid, thread]
                              {
            for (int iteration = 0; iteration < 5; ++iteration)
            {
                FFTWRealToComplexBatchFloat batch (64 + 2 * thread, 2);
                for (int transform = 0; transform < batch.getTransformCount(); ++transform)
                    for (int sample = 0; sample < batch.getTransformLength(); ++sample)
                        batch.getInputPointer (transform)[sample] = 1.0f;
                batch.execute();
                if (std::abs (batch.getOutputPointer (0)[0].real()
                              - static_cast<float> (batch.getTransformLength()))
                    > 1.0e-4f)
                    valid.store (false);
            } });
    }
    for (auto& thread : threads)
        thread.join();
    EXPECT_TRUE (valid.load());
}
} // namespace
