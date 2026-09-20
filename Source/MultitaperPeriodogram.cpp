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

#include "MultitaperPeriodogram.h"

#include <algorithm>
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

    bool validMode (DetrendMode mode)
    {
        return mode == DetrendMode::none || mode == DetrendMode::mean || mode == DetrendMode::linear;
    }

    int checkedInt (std::size_t value, const char* message)
    {
        if (value == 0 || value > static_cast<std::size_t> (std::numeric_limits<int>::max()))
            throw std::invalid_argument (message);
        return static_cast<int> (value);
    }

    std::size_t checkedProduct (std::size_t left,
                                std::size_t right,
                                const char* message)
    {
        if (left == 0 || right == 0
            || left > std::numeric_limits<std::size_t>::max() / right)
            throw std::invalid_argument (message);
        return left * right;
    }
} // namespace

MultitaperPeriodogram::MultitaperPeriodogram (
    std::size_t numChannels,
    double sampleRate,
    std::shared_ptr<const DpssTaperBank> taperBank,
    DetrendMode detrendMode,
    unsigned int planningFlags)
    : channelCount (numChannels),
      sampleCount (0),
      taperCount (0),
      binCount (0),
      sampleRateHz (sampleRate),
      centredTimeSquareSum (0.0),
      mode (detrendMode),
      bank (std::move (taperBank))
{
    checkedInt (channelCount, "Channel count is out of range");
    if (bank == nullptr || ! bank->succeeded()
        || ! std::isfinite (sampleRateHz) || sampleRateHz <= 0.0
        || ! validMode (mode))
        throw std::invalid_argument ("MultitaperPeriodogram configuration is invalid");

    sampleCount = bank->sampleCount;
    taperCount = bank->taperCount;
    const auto checkedSamples = checkedInt (sampleCount, "Sample count is out of range");
    checkedInt (taperCount, "Taper count is out of range");
    const auto taperElements = checkedProduct (
        sampleCount, taperCount, "Taper-bank dimensions are out of range");
    if (bank->tapers.size() != taperElements
        || bank->concentrationRatios.size() != taperCount)
        throw std::invalid_argument ("Taper-bank storage is malformed");

    binCount = sampleCount / 2 + 1;
    const auto outputElements = checkedProduct (
        channelCount, binCount, "PSD output dimensions are out of range");
    const auto transformCount = checkedProduct (
        channelCount, taperCount, "FFT batch dimensions are out of range");
    const auto checkedTransforms = checkedInt (
        transformCount, "FFT transform count is out of range");

    taperPointers.resize (taperCount);
    inverseNormalizations.resize (taperCount);
    for (std::size_t taper = 0; taper < taperCount; ++taper)
    {
        const auto concentration = bank->concentrationRatios[taper];
        if (! std::isfinite (concentration) || concentration < 0.0 || concentration > 1.0)
            throw std::invalid_argument ("Taper concentration ratio is invalid");

        double energy = 0.0;
        const auto* coefficients = bank->getTaper (taper);
        if (coefficients == nullptr)
            throw std::invalid_argument ("Taper-bank storage is malformed");
        taperPointers[taper] = coefficients;
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            const auto coefficient = coefficients[sample];
            if (! std::isfinite (coefficient))
                throw std::invalid_argument ("Taper coefficients must be finite");
            energy += static_cast<double> (coefficient) * static_cast<double> (coefficient);
        }

        const auto normalization = sampleRateHz * energy * static_cast<double> (taperCount);
        if (! std::isfinite (normalization) || normalization <= 0.0)
            throw std::invalid_argument ("Taper normalization must be finite and positive");
        inverseNormalizations[taper] = 1.0 / normalization;
    }

    const auto centre = 0.5 * static_cast<double> (sampleCount - 1);
    for (std::size_t sample = 0; sample < sampleCount; ++sample)
    {
        const auto centredTime = static_cast<double> (sample) - centre;
        centredTimeSquareSum += centredTime * centredTime;
    }

    psd.resize (outputElements);
    workingPsd.resize (outputElements);
    detrendedTile.resize (std::min<std::size_t> (sampleCount, 1024));
    transform = std::make_unique<FFTWRealToComplexBatchFloat> (
        checkedSamples, checkedTransforms, planningFlags);
    fftInputs.resize (transformCount);
    fftOutputs.resize (transformCount);
    for (std::size_t index = 0; index < transformCount; ++index)
    {
        fftInputs[index] = transform->getInputPointer (static_cast<int> (index));
        fftOutputs[index] = transform->getOutputPointer (static_cast<int> (index));
    }
}

bool MultitaperPeriodogram::compute (const ChannelSampleView* channels,
                                     std::size_t numChannels) noexcept
{
    if (channels == nullptr || numChannels != channelCount)
        return false;

    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto& view = channels[channel];
        if (view.firstData == nullptr || view.firstSize > sampleCount
            || view.secondSize != sampleCount - view.firstSize
            || (view.secondSize > 0 && view.secondData == nullptr))
            return false;
    }

    const auto centre = 0.5 * static_cast<double> (sampleCount - 1);
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto& view = channels[channel];
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

        for (std::size_t tileStart = 0; tileStart < sampleCount; tileStart += detrendedTile.size())
        {
            const auto tileCount = std::min (detrendedTile.size(), sampleCount - tileStart);
            auto logicalIndex = tileStart;
            std::size_t tileIndex = 0;
            while (tileIndex < tileCount)
            {
                const auto inFirstRegion = logicalIndex < view.firstSize;
                const auto regionOffset = inFirstRegion
                                              ? logicalIndex
                                              : logicalIndex - view.firstSize;
                const auto regionSize = inFirstRegion ? view.firstSize : view.secondSize;
                const auto* source = (inFirstRegion ? view.firstData : view.secondData)
                                     + regionOffset;
                const auto count = std::min (
                    tileCount - tileIndex, regionSize - regionOffset);
                for (std::size_t index = 0; index < count; ++index, ++logicalIndex, ++tileIndex)
                {
                    if (! std::isfinite (source[index]))
                        return false;
                    auto value = source[index];
                    if (mode != DetrendMode::none)
                    {
                        const auto fitted = trend.intercept
                                            + trend.slope * (static_cast<double> (logicalIndex) - centre);
                        value -= static_cast<float> (fitted);
                    }
                    detrendedTile[tileIndex] = value;
                }
            }

            for (std::size_t taper = 0; taper < taperCount; ++taper)
            {
                const auto transformIndex = channel * taperCount + taper;
                auto* destination = fftInputs[transformIndex] + tileStart;
                const auto* coefficients = taperPointers[taper] + tileStart;
                for (std::size_t index = 0; index < tileCount; ++index)
                {
                    destination[index] = detrendedTile[index] * coefficients[index];
                }
            }
        }
    }

    transform->execute();
    const auto hasNyquistBin = sampleCount % 2 == 0;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        auto* output = workingPsd.data() + channel * binCount;
        for (std::size_t bin = 0; bin < binCount; ++bin)
        {
            double power = 0.0;
            for (std::size_t taper = 0; taper < taperCount; ++taper)
            {
                const auto transformIndex = channel * taperCount + taper;
                const auto value = fftOutputs[transformIndex][bin];
                const auto real = static_cast<double> (value.real());
                const auto imaginary = static_cast<double> (value.imag());
                power += (real * real + imaginary * imaginary)
                         * inverseNormalizations[taper];
            }

            const auto isNyquist = hasNyquistBin && bin == sampleCount / 2;
            if (bin != 0 && ! isNyquist)
                power *= 2.0;
            if (! std::isfinite (power)
                || power > static_cast<double> (std::numeric_limits<float>::max()))
                return false;
            output[bin] = static_cast<float> (power);
        }
    }

    psd.swap (workingPsd);
    return true;
}

const float* MultitaperPeriodogram::getChannelData (std::size_t channel) const noexcept
{
    return channel < channelCount ? psd.data() + channel * binCount : nullptr;
}
} // namespace spectrumviewer
