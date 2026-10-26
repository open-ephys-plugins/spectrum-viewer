/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "SpectrumAmplitudeRange.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace spectrumviewer
{
namespace
{
float percentile (std::vector<float>& values, double fraction)
{
    const auto index = static_cast<std::size_t> (
        std::floor (fraction * static_cast<double> (values.size() - 1)));
    std::nth_element (values.begin(),
                      values.begin() + static_cast<std::ptrdiff_t> (index),
                      values.end());
    return values[index];
}
} // namespace

void SpectrumAmplitudeRange::setMode (AmplitudeRangeMode newMode) noexcept
{
    if (newMode != AmplitudeRangeMode::automatic
        && newMode != AmplitudeRangeMode::fixed)
        return;
    if (mode != newMode && newMode == AmplitudeRangeMode::automatic)
        automaticRangeIsValid = false;
    mode = newMode;
}

bool SpectrumAmplitudeRange::setFixedRange (float minimumDb, float maximumDb) noexcept
{
    if (! std::isfinite (minimumDb) || ! std::isfinite (maximumDb)
        || maximumDb - minimumDb < minimumSpanDb)
        return false;
    fixedRange = { minimumDb, maximumDb };
    return true;
}

DecibelRange SpectrumAmplitudeRange::getCurrentRange() const noexcept
{
    if (mode == AmplitudeRangeMode::automatic && automaticRangeIsValid)
        return automaticRange;
    return fixedRange;
}

DecibelRange SpectrumAmplitudeRange::update (
    const std::vector<std::vector<float>>& meanDb,
    const std::vector<std::vector<float>>& peakDb,
    std::size_t channelCount,
    double elapsedSignalSeconds)
{
    if (mode == AmplitudeRangeMode::fixed)
        return fixedRange;

    DecibelRange target;
    if (! fitTarget (meanDb, peakDb, channelCount, target))
        return getCurrentRange();
    if (! automaticRangeIsValid)
    {
        automaticRange = target;
        automaticRangeIsValid = true;
        return automaticRange;
    }

    automaticRange.minimum = follow (automaticRange.minimum,
                                     target.minimum,
                                     target.minimum < automaticRange.minimum,
                                     elapsedSignalSeconds);
    automaticRange.maximum = follow (automaticRange.maximum,
                                     target.maximum,
                                     target.maximum > automaticRange.maximum,
                                     elapsedSignalSeconds);
    return automaticRange;
}

bool SpectrumAmplitudeRange::fitTarget (
    const std::vector<std::vector<float>>& meanDb,
    const std::vector<std::vector<float>>& peakDb,
    std::size_t channelCount,
    DecibelRange& target)
{
    const auto usableChannels = std::min ({ channelCount, meanDb.size(), peakDb.size() });
    finiteMeans.clear();
    auto peakMaximum = -std::numeric_limits<float>::infinity();
    for (std::size_t channel = 0; channel < usableChannels; ++channel)
    {
        for (const auto value : meanDb[channel])
            if (std::isfinite (value))
                finiteMeans.push_back (value);
        for (const auto value : peakDb[channel])
            if (std::isfinite (value))
                peakMaximum = std::max (peakMaximum, value);
    }
    if (finiteMeans.empty())
        return false;

    const auto lower = percentile (finiteMeans, 0.02);
    const auto upperMean = percentile (finiteMeans, 0.98);
    const auto upper = std::isfinite (peakMaximum)
                           ? std::max (upperMean, peakMaximum)
                           : upperMean;
    target = { lower - paddingDb, upper + paddingDb };
    if (target.maximum - target.minimum < minimumSpanDb)
    {
        const auto centre = 0.5f * (target.minimum + target.maximum);
        target.minimum = centre - 0.5f * minimumSpanDb;
        target.maximum = centre + 0.5f * minimumSpanDb;
    }
    return true;
}

float SpectrumAmplitudeRange::follow (float current,
                                      float target,
                                      bool movingOutward,
                                      double elapsedSignalSeconds) noexcept
{
    if (std::abs (target - current) <= deadbandDb
        || ! std::isfinite (elapsedSignalSeconds) || elapsedSignalSeconds <= 0.0)
        return current;
    const auto timeConstant = movingOutward
                                  ? outwardTimeConstantSeconds
                                  : inwardTimeConstantSeconds;
    const auto alpha = 1.0 - std::exp (-elapsedSignalSeconds / timeConstant);
    return static_cast<float> (static_cast<double> (current)
                               + alpha * static_cast<double> (target - current));
}
} // namespace spectrumviewer
