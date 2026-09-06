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

    ------------------------------------------------------------------
*/

#include "ReferencePeriodogram.h"

#include <cmath>
#include <complex>
#include <limits>
#include <stdexcept>

namespace spectrumviewer
{
namespace
{
    constexpr double twoPi = 6.283185307179586476925286766559;

    struct LinearTrend
    {
        double intercept = 0.0;
        double slope = 0.0;
    };

    LinearTrend estimateTrend (const float* samples,
                               std::size_t numSamples,
                               DetrendMode mode)
    {
        if (mode == DetrendMode::none)
            return {};

        double sum = 0.0;
        for (std::size_t sample = 0; sample < numSamples; ++sample)
            sum += static_cast<double> (samples[sample]);

        LinearTrend trend;
        trend.intercept = sum / static_cast<double> (numSamples);
        if (mode == DetrendMode::mean || numSamples == 1)
            return trend;

        const auto centre = 0.5 * static_cast<double> (numSamples - 1);
        double timeSquaredSum = 0.0;
        double timeSampleSum = 0.0;
        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            const auto centredTime = static_cast<double> (sample) - centre;
            timeSquaredSum += centredTime * centredTime;
            timeSampleSum += centredTime * static_cast<double> (samples[sample]);
        }

        trend.slope = timeSampleSum / timeSquaredSum;
        return trend;
    }

    double detrendedSample (float sample,
                            std::size_t index,
                            std::size_t numSamples,
                            DetrendMode mode,
                            const LinearTrend& trend)
    {
        if (mode == DetrendMode::none)
            return static_cast<double> (sample);

        const auto centre = 0.5 * static_cast<double> (numSamples - 1);
        return static_cast<double> (sample)
               - trend.intercept
               - trend.slope * (static_cast<double> (index) - centre);
    }

    std::size_t checkedOutputSize (std::size_t numChannels, std::size_t numBins)
    {
        if (numChannels > std::numeric_limits<std::size_t>::max() / numBins)
            throw std::length_error ("ReferencePeriodogram output is too large");
        return numChannels * numBins;
    }
} // namespace

PeriodogramResult ReferencePeriodogram::compute (const float* planarSamples,
                                                 std::size_t numChannels,
                                                 std::size_t channelStride,
                                                 std::size_t numSamples,
                                                 double sampleRate,
                                                 const std::vector<double>& taper,
                                                 DetrendMode detrendMode)
{
    const auto validDetrendMode = detrendMode == DetrendMode::none
                                  || detrendMode == DetrendMode::mean
                                  || detrendMode == DetrendMode::linear;
    if (planarSamples == nullptr || numChannels == 0 || numSamples == 0
        || channelStride < numSamples || taper.size() != numSamples
        || ! std::isfinite (sampleRate) || sampleRate <= 0.0
        || ! validDetrendMode
        || numChannels > std::numeric_limits<std::size_t>::max() / channelStride)
        throw std::invalid_argument ("ReferencePeriodogram configuration is invalid");

    double taperEnergy = 0.0;
    for (const auto coefficient : taper)
    {
        if (! std::isfinite (coefficient))
            throw std::invalid_argument ("ReferencePeriodogram taper must be finite");
        taperEnergy += coefficient * coefficient;
    }
    if (! std::isfinite (taperEnergy) || taperEnergy <= 0.0)
        throw std::invalid_argument ("ReferencePeriodogram taper energy must be positive");

    PeriodogramResult result;
    result.numChannels = numChannels;
    result.numSamples = numSamples;
    result.numBins = numSamples / 2 + 1;
    result.sampleRate = sampleRate;
    result.binWidth = sampleRate / static_cast<double> (numSamples);
    result.psd.resize (checkedOutputSize (numChannels, result.numBins));

    const auto normalization = sampleRate * taperEnergy;
    if (! std::isfinite (normalization))
        throw std::invalid_argument ("ReferencePeriodogram normalization must be finite");
    const auto hasNyquistBin = numSamples % 2 == 0;
    std::vector<double> preprocessed (numSamples);

    for (std::size_t channel = 0; channel < numChannels; ++channel)
    {
        const auto* samples = planarSamples + channel * channelStride;
        const auto trend = estimateTrend (samples, numSamples, detrendMode);
        auto* output = result.psd.data() + channel * result.numBins;

        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            if (! std::isfinite (samples[sample]))
                throw std::invalid_argument ("ReferencePeriodogram samples must be finite");
            preprocessed[sample] = taper[sample]
                                   * detrendedSample (samples[sample], sample, numSamples, detrendMode, trend);
        }

        for (std::size_t bin = 0; bin < result.numBins; ++bin)
        {
            std::complex<double> transform {};
            for (std::size_t sample = 0; sample < numSamples; ++sample)
            {
                const auto phase = -twoPi * static_cast<double> (bin)
                                   * static_cast<double> (sample) / static_cast<double> (numSamples);
                transform += preprocessed[sample]
                             * std::complex<double> (std::cos (phase), std::sin (phase));
            }

            auto power = std::norm (transform) / normalization;
            const auto isNyquist = hasNyquistBin && bin == numSamples / 2;
            if (bin != 0 && ! isNyquist)
                power *= 2.0;
            output[bin] = power;
        }
    }

    return result;
}
} // namespace spectrumviewer
