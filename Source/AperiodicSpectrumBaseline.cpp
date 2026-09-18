/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "AperiodicSpectrumBaseline.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace spectrumviewer
{
AperiodicSpectrumBaseline::AperiodicSpectrumBaseline (
    std::size_t maximumInputBins,
    std::size_t maximumOutputBins)
    : inputCapacity (maximumInputBins),
      outputCapacity (maximumOutputBins),
      bandScratch (maximumInputBins),
      knotLogFrequencies (maximumKnots),
      knotValuesDb (maximumKnots),
      smoothedKnotValuesDb (maximumKnots)
{
    if (maximumInputBins == 0 || maximumOutputBins == 0)
        throw std::invalid_argument ("Aperiodic baseline dimensions are invalid");
}

bool AperiodicSpectrumBaseline::estimate (
    const float* planarPsd,
    std::size_t numChannels,
    std::size_t numBins,
    double sampleRateHz,
    std::size_t windowSampleCount,
    const float* outputFrequenciesHz,
    std::size_t outputBinCount,
    float* planarBaselineDb) noexcept
{
    if (planarPsd == nullptr || outputFrequenciesHz == nullptr
        || planarBaselineDb == nullptr || numChannels == 0 || numBins < 3
        || numBins > inputCapacity || outputBinCount == 0
        || outputBinCount > outputCapacity || windowSampleCount == 0
        || numBins != windowSampleCount / 2 + 1
        || ! std::isfinite (sampleRateHz) || sampleRateHz <= 0.0)
        return false;

    const auto binWidth = sampleRateHz / static_cast<double> (windowSampleCount);
    const auto minimumFrequency = std::max (10.0, 2.0 * binWidth);
    const auto maximumFrequency = sampleRateHz * 0.5;
    if (minimumFrequency >= maximumFrequency)
        return false;

    constexpr auto bandsPerDecade = 10.0;
    const auto decades = std::log10 (maximumFrequency / minimumFrequency);
    const auto knotCount = std::min (
        maximumKnots,
        std::max<std::size_t> (3, static_cast<std::size_t> (
                                     std::ceil (decades * bandsPerDecade))));
    const auto logMinimum = std::log (minimumFrequency);
    const auto logMaximum = std::log (maximumFrequency);

    for (std::size_t knot = 0; knot < knotCount; ++knot)
    {
        const auto fraction = (static_cast<double> (knot) + 0.5)
                              / static_cast<double> (knotCount);
        knotLogFrequencies[knot] = static_cast<float> (
            logMinimum + fraction * (logMaximum - logMinimum));
    }

    for (std::size_t channel = 0; channel < numChannels; ++channel)
    {
        auto validKnots = std::size_t { 0 };
        for (std::size_t knot = 0; knot < knotCount; ++knot)
        {
            const auto edge0 = std::exp (
                logMinimum + static_cast<double> (knot) / static_cast<double> (knotCount)
                                 * (logMaximum - logMinimum));
            const auto edge1 = std::exp (
                logMinimum + static_cast<double> (knot + 1) / static_cast<double> (knotCount)
                                 * (logMaximum - logMinimum));
            const auto firstBin = std::max<std::size_t> (
                1, static_cast<std::size_t> (std::ceil (edge0 / binWidth)));
            const auto lastBin = std::min<std::size_t> (
                numBins - 1,
                static_cast<std::size_t> (std::floor (edge1 / binWidth)));
            auto valueCount = std::size_t { 0 };
            if (lastBin >= firstBin)
                for (auto bin = firstBin; bin <= lastBin; ++bin)
                {
                    const auto power = planarPsd[channel * numBins + bin];
                    if (std::isfinite (power) && power > 0.0f)
                        bandScratch[valueCount++] = 10.0f * std::log10 (power);
                }
            if (valueCount == 0)
                continue;

            const auto middle = bandScratch.begin()
                                + static_cast<std::ptrdiff_t> (valueCount / 2);
            std::nth_element (bandScratch.begin(), middle,
                              bandScratch.begin()
                                  + static_cast<std::ptrdiff_t> (valueCount));
            knotLogFrequencies[validKnots] = static_cast<float> (
                0.5 * (std::log (edge0) + std::log (edge1)));
            knotValuesDb[validKnots] = *middle;
            ++validKnots;
        }

        if (validKnots < 2)
            return false;

        smoothedKnotValuesDb[0] = knotValuesDb[0];
        smoothedKnotValuesDb[validKnots - 1] = knotValuesDb[validKnots - 1];
        for (std::size_t knot = 1; knot + 1 < validKnots; ++knot)
        {
            float neighbours[] { knotValuesDb[knot - 1], knotValuesDb[knot],
                                 knotValuesDb[knot + 1] };
            std::sort (neighbours, neighbours + 3);
            smoothedKnotValuesDb[knot] = neighbours[1];
        }

        auto upperKnot = std::size_t { 1 };
        for (std::size_t output = 0; output < outputBinCount; ++output)
        {
            const auto frequency = static_cast<double> (outputFrequenciesHz[output]);
            auto baseline = std::numeric_limits<float>::quiet_NaN();
            if (std::isfinite (frequency) && frequency >= minimumFrequency
                && frequency <= maximumFrequency)
            {
                const auto logFrequency = static_cast<float> (std::log (frequency));
                while (upperKnot + 1 < validKnots
                       && knotLogFrequencies[upperKnot] < logFrequency)
                    ++upperKnot;
                if (logFrequency <= knotLogFrequencies[0])
                    baseline = smoothedKnotValuesDb[0];
                else if (logFrequency >= knotLogFrequencies[validKnots - 1])
                    baseline = smoothedKnotValuesDb[validKnots - 1];
                else
                {
                    const auto lowerKnot = upperKnot - 1;
                    const auto fraction = (logFrequency - knotLogFrequencies[lowerKnot])
                                          / (knotLogFrequencies[upperKnot]
                                             - knotLogFrequencies[lowerKnot]);
                    baseline = smoothedKnotValuesDb[lowerKnot]
                               + fraction * (smoothedKnotValuesDb[upperKnot]
                                             - smoothedKnotValuesDb[lowerKnot]);
                }
            }
            planarBaselineDb[channel * outputBinCount + output] = baseline;
        }
    }
    return true;
}
} // namespace spectrumviewer
