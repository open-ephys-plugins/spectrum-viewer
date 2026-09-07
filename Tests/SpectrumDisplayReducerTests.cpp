#include "gtest/gtest.h"

#include "SpectrumDisplayReducer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace
{
using spectrumviewer::FrequencyScale;
using spectrumviewer::SpectrumDisplayReducer;

TEST (SpectrumDisplayReducerTests, FlatPsdRemainsFlatAcrossLinearColumns)
{
    SpectrumDisplayReducer reducer (2, 9, 4);
    std::array<float, 18> psd {};
    std::fill (psd.begin(), psd.end(), 3.5f);
    ASSERT_TRUE (reducer.reduce (psd.data(), 2, 9, 16.0, 16, 4, FrequencyScale::linear, 0.0, 8.0));
    const auto& view = reducer.getView();
    ASSERT_EQ (view.numColumns, 4u);
    EXPECT_DOUBLE_EQ (view.minimumFrequencyHz, 0.0);
    EXPECT_DOUBLE_EQ (view.maximumFrequencyHz, 8.0);
    for (std::size_t channel = 0; channel < 2; ++channel)
        for (std::size_t column = 0; column < 4; ++column)
        {
            EXPECT_FLOAT_EQ (view.getChannelMean (channel)[column], 3.5f);
            EXPECT_FLOAT_EQ (view.getChannelPeak (channel)[column], 3.5f);
        }
}

TEST (SpectrumDisplayReducerTests, AreaWeightedMeanConservesIntegratedPower)
{
    SpectrumDisplayReducer reducer (1, 5, 1);
    const std::array<float, 5> psd { 2.0f, 4.0f, 6.0f, 8.0f, 10.0f };
    ASSERT_TRUE (reducer.reduce (psd.data(), 1, 5, 8.0, 8, 1, FrequencyScale::linear, 0.0, 4.0));

    // DC and Nyquist represent half-width support cells. Their 0.5 + 3 + 0.5 Hz
    // weighted integral is 24, so the mean over the 4 Hz display column is 6.
    EXPECT_FLOAT_EQ (reducer.getView().getChannelMean (0)[0], 6.0f);
}

TEST (SpectrumDisplayReducerTests, PeakEnvelopePreservesANarrowLine)
{
    SpectrumDisplayReducer reducer (1, 9, 2);
    std::array<float, 9> psd {};
    psd.fill (1.0f);
    psd[3] = 101.0f;
    ASSERT_TRUE (reducer.reduce (psd.data(), 1, 9, 16.0, 16, 2, FrequencyScale::linear, 0.0, 8.0));
    const auto& view = reducer.getView();
    EXPECT_GT (view.getChannelPeak (0)[0], view.getChannelMean (0)[0]);
    EXPECT_FLOAT_EQ (*std::max_element (view.getChannelPeak (0),
                                        view.getChannelPeak (0) + view.numColumns),
                     101.0f);
}

TEST (SpectrumDisplayReducerTests, LogarithmicColumnsAreMonotonicAndExcludeDc)
{
    SpectrumDisplayReducer reducer (1, 9, 8);
    std::array<float, 9> psd {};
    psd.fill (2.0f);
    ASSERT_TRUE (reducer.reduce (psd.data(), 1, 9, 16.0, 16, 8, FrequencyScale::logarithmic, 0.0, 8.0));
    const auto& view = reducer.getView();
    EXPECT_GT (view.frequenciesHz[0], 0.0f);
    for (std::size_t column = 1; column < view.numColumns; ++column)
        EXPECT_GT (view.frequenciesHz[column], view.frequenciesHz[column - 1]);
}

TEST (SpectrumDisplayReducerTests, SupportsOddLengthNyquistExtent)
{
    SpectrumDisplayReducer reducer (1, 8, 4);
    std::array<float, 8> psd {};
    psd.fill (4.0f);
    ASSERT_TRUE (reducer.reduce (psd.data(), 1, 8, 15.0, 15, 4, FrequencyScale::linear, 0.0, 7.5));
    const auto& view = reducer.getView();
    EXPECT_FLOAT_EQ (view.getChannelMean (0)[3], 4.0f);
    EXPECT_LT (view.frequenciesHz[3], 7.5f);
}
} // namespace
