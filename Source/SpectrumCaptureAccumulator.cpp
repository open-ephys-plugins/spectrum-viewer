/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "SpectrumCaptureAccumulator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace spectrumviewer
{
SpectrumCaptureAccumulator::SpectrumCaptureAccumulator (
    std::size_t channelCount,
    std::size_t binCount,
    std::size_t targetWindowCount)
    : numChannels (channelCount),
      numBins (binCount),
      targetWindows (targetWindowCount),
      means (checkedValueCount (channelCount, binCount), 0.0f),
      sumSquaredDifferences (means.size(), 0.0f)
{
    if (targetWindowCount == 0)
        throw std::invalid_argument ("Spectrum capture target must be positive");
}

bool SpectrumCaptureAccumulator::add (const float* planarPsd,
                                      std::size_t channelCount,
                                      std::size_t binCount) noexcept
{
    if (planarPsd == nullptr || channelCount != numChannels || binCount != numBins
        || isComplete())
    {
        ++rejectedWindows;
        return false;
    }

    for (std::size_t index = 0; index < means.size(); ++index)
    {
        if (! std::isfinite (planarPsd[index]) || planarPsd[index] < 0.0f)
        {
            ++rejectedWindows;
            return false;
        }
    }

    const auto nextCount = includedWindows + 1;
    const auto reciprocalCount = 1.0f / static_cast<float> (nextCount);
    for (std::size_t index = 0; index < means.size(); ++index)
    {
        const auto delta = planarPsd[index] - means[index];
        means[index] += delta * reciprocalCount;
        const auto updatedDelta = planarPsd[index] - means[index];
        sumSquaredDifferences[index] += delta * updatedDelta;
    }
    includedWindows = nextCount;
    return true;
}

void SpectrumCaptureAccumulator::reset() noexcept
{
    includedWindows = 0;
    rejectedWindows = 0;
    std::fill (means.begin(), means.end(), 0.0f);
    std::fill (sumSquaredDifferences.begin(), sumSquaredDifferences.end(), 0.0f);
}

const float* SpectrumCaptureAccumulator::getChannelMean (std::size_t channel) const noexcept
{
    return channel < numChannels ? means.data() + channel * numBins : nullptr;
}

float SpectrumCaptureAccumulator::getSampleVariance (std::size_t channel,
                                                     std::size_t bin) const noexcept
{
    if (channel >= numChannels || bin >= numBins || includedWindows < 2)
        return 0.0f;
    return std::max (0.0f,
                     sumSquaredDifferences[channel * numBins + bin]
                         / static_cast<float> (includedWindows - 1));
}

std::size_t SpectrumCaptureAccumulator::checkedValueCount (
    std::size_t channelCount,
    std::size_t binCount)
{
    if (channelCount == 0 || binCount == 0)
        throw std::invalid_argument ("Spectrum capture dimensions must be positive");
    if (channelCount > std::numeric_limits<std::size_t>::max() / binCount)
        throw std::length_error ("Spectrum capture allocation is too large");
    return channelCount * binCount;
}
} // namespace spectrumviewer
