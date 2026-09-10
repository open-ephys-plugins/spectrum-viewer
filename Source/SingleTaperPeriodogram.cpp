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

#include "SingleTaperPeriodogram.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace spectrumviewer
{
namespace
{
    struct LinearTrend
    {
        double intercept = 0.0;
        double slope = 0.0;
    };

    int checkedInt (std::size_t value, const char* message)
    {
        if (value == 0 || value > static_cast<std::size_t> (std::numeric_limits<int>::max()))
            throw std::invalid_argument (message);
        return static_cast<int> (value);
    }

    std::size_t checkedBinCount (std::size_t samples)
    {
        checkedInt (samples, "Sample count is out of range");
        return samples / 2 + 1;
    }

    std::size_t checkedOutputSize (std::size_t channels, std::size_t bins)
    {
        if (channels == 0 || bins == 0
            || channels > std::numeric_limits<std::size_t>::max() / bins)
            throw std::invalid_argument ("Periodogram output dimensions are out of range");
        return channels * bins;
    }

    bool validMode (DetrendMode mode)
    {
        return mode == DetrendMode::none || mode == DetrendMode::mean || mode == DetrendMode::linear;
    }
} // namespace

SingleTaperPeriodogram::SingleTaperPeriodogram (std::size_t numChannels,
                                                std::size_t numSamples,
                                                double sampleRate,
                                                std::vector<float> taper,
                                                DetrendMode detrendMode,
                                                unsigned int planningFlags)
    : channelCount (numChannels),
      sampleCount (numSamples),
      binCount (checkedBinCount (numSamples)),
      sampleRateHz (sampleRate),
      normalization (0.0),
      centredTimeSquareSum (0.0),
      mode (detrendMode),
      taperCoefficients (std::move (taper)),
      psd (checkedOutputSize (numChannels, binCount))
{
    const auto checkedChannels = checkedInt (channelCount, "Channel count is out of range");
    const auto checkedSamples = checkedInt (sampleCount, "Sample count is out of range");
    if (! std::isfinite (sampleRateHz) || sampleRateHz <= 0.0
        || taperCoefficients.size() != sampleCount || ! validMode (mode))
        throw std::invalid_argument ("SingleTaperPeriodogram configuration is invalid");

    double taperEnergy = 0.0;
    for (const auto coefficient : taperCoefficients)
    {
        if (! std::isfinite (coefficient))
            throw std::invalid_argument ("Taper coefficients must be finite");
        taperEnergy += static_cast<double> (coefficient) * static_cast<double> (coefficient);
    }
    normalization = sampleRateHz * taperEnergy;
    if (! std::isfinite (normalization) || normalization <= 0.0)
        throw std::invalid_argument ("Taper normalization must be finite and positive");

    const auto centre = 0.5 * static_cast<double> (sampleCount - 1);
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        const auto centredTime = static_cast<double> (sample) - centre;
        centredTimeSquareSum += centredTime * centredTime;
    }

    transform = std::make_unique<FFTWRealToComplexBatchFloat> (
        checkedSamples, checkedChannels, planningFlags);
}

bool SingleTaperPeriodogram::compute (const ChannelSampleView* channels,
                                      std::size_t numChannels) noexcept
{
    if (channels == nullptr || numChannels != channelCount)
        return false;

    const auto centre = 0.5 * static_cast<double> (sampleCount - 1);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto& view = channels[channel];
        if (view.firstData == nullptr || view.firstSize > sampleCount
            || view.secondSize != sampleCount - view.firstSize
            || (view.secondSize > 0 && view.secondData == nullptr))
            return false;

        LinearTrend trend;
        if (mode != DetrendMode::none)
        {
            double sum = 0.0;
            double centredProduct = 0.0;
            std::size_t logicalIndex = 0;
            const auto accumulate = [&] (const float* data, std::size_t count)
            {
                for (std::size_t index = 0; index < count; ++index, ++logicalIndex)
                {
                    const auto value = static_cast<double> (data[index]);
                    if (! std::isfinite (value))
                        return false;
                    sum += value;
                    if (mode == DetrendMode::linear)
                        centredProduct += (static_cast<double> (logicalIndex) - centre) * value;
                }
                return true;
            };

            if (! accumulate (view.firstData, view.firstSize)
                || ! accumulate (view.secondData, view.secondSize))
                return false;

            trend.intercept = sum / static_cast<double> (sampleCount);
            if (mode == DetrendMode::linear && centredTimeSquareSum > 0.0)
                trend.slope = centredProduct / centredTimeSquareSum;
        }

        auto* fftInput = transform->getInputPointer (static_cast<int> (channel));
        std::size_t logicalIndex = 0;
        const auto preprocess = [&] (const float* data, std::size_t count)
        {
            for (std::size_t index = 0; index < count; ++index, ++logicalIndex)
            {
                if (! std::isfinite (data[index]))
                    return false;
                auto value = data[index];
                if (mode != DetrendMode::none)
                {
                    const auto fitted = trend.intercept
                                        + trend.slope * (static_cast<double> (logicalIndex) - centre);
                    value -= static_cast<float> (fitted);
                }
                fftInput[logicalIndex] = value * taperCoefficients[logicalIndex];
            }
            return true;
        };

        if (! preprocess (view.firstData, view.firstSize)
            || ! preprocess (view.secondData, view.secondSize))
            return false;
    }

    transform->execute();
    const auto hasNyquistBin = sampleCount % 2 == 0;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto* fftOutput = transform->getOutputPointer (static_cast<int> (channel));
        auto* output = psd.data() + channel * binCount;
        for (std::size_t bin = 0; bin < binCount; ++bin)
        {
            const auto real = static_cast<double> (fftOutput[bin].real());
            const auto imaginary = static_cast<double> (fftOutput[bin].imag());
            auto power = (real * real + imaginary * imaginary) / normalization;
            const auto isNyquist = hasNyquistBin && bin == sampleCount / 2;
            if (bin != 0 && ! isNyquist)
                power *= 2.0;
            output[bin] = static_cast<float> (power);
        }
    }
    return true;
}
} // namespace spectrumviewer
