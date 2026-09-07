/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "SpectrumDisplayReducer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace spectrumviewer
{
namespace
{
    std::size_t checkedOutputCount (std::size_t channels, std::size_t columns)
    {
        if (channels == 0 || columns == 0
            || channels > std::numeric_limits<std::size_t>::max() / columns)
            throw std::invalid_argument ("SpectrumDisplayReducer dimensions are invalid");
        return channels * columns;
    }
} // namespace

const float* SpectrumDisplayReducer::View::getChannelMean (std::size_t channel) const noexcept
{
    return channel < numChannels ? means + channel * columnStride : nullptr;
}

const float* SpectrumDisplayReducer::View::getChannelPeak (std::size_t channel) const noexcept
{
    return channel < numChannels ? peaks + channel * columnStride : nullptr;
}

SpectrumDisplayReducer::SpectrumDisplayReducer (std::size_t maximumChannels,
                                                std::size_t maximumInputBins,
                                                std::size_t maximumColumns)
    : channelCapacity (maximumChannels),
      inputBinCapacity (maximumInputBins),
      columnCapacity (maximumColumns),
      means (checkedOutputCount (maximumChannels, maximumColumns)),
      peaks (checkedOutputCount (maximumChannels, maximumColumns)),
      frequencies (maximumColumns)
{
    if (maximumInputBins == 0)
        throw std::invalid_argument ("SpectrumDisplayReducer dimensions are invalid");
}

bool SpectrumDisplayReducer::reduce (const float* planarPsd,
                                     std::size_t numChannels,
                                     std::size_t numBins,
                                     double sampleRateHz,
                                     std::size_t windowSampleCount,
                                     std::size_t requestedColumns,
                                     FrequencyScale scale,
                                     double minimumFrequencyHz,
                                     double maximumFrequencyHz) noexcept
{
    view = {};
    if (planarPsd == nullptr || numChannels == 0 || numChannels > channelCapacity
        || numBins == 0 || numBins > inputBinCapacity || windowSampleCount == 0
        || numBins != windowSampleCount / 2 + 1 || requestedColumns == 0
        || ! std::isfinite (sampleRateHz) || sampleRateHz <= 0.0
        || ! std::isfinite (minimumFrequencyHz) || ! std::isfinite (maximumFrequencyHz)
        || (scale != FrequencyScale::linear && scale != FrequencyScale::logarithmic))
        return false;

    const auto binWidth = sampleRateHz / static_cast<double> (windowSampleCount);
    const auto nyquist = sampleRateHz * 0.5;
    auto lower = std::max (0.0, minimumFrequencyHz);
    auto upper = std::min (nyquist, maximumFrequencyHz);
    if (scale == FrequencyScale::logarithmic)
        lower = std::max (lower, binWidth);
    if (! std::isfinite (lower) || ! std::isfinite (upper) || upper <= lower)
        return false;

    const auto columns = std::min ({ requestedColumns, columnCapacity, numBins });
    const auto logLower = scale == FrequencyScale::logarithmic ? std::log (lower) : 0.0;
    const auto logUpper = scale == FrequencyScale::logarithmic ? std::log (upper) : 0.0;

    for (std::size_t column = 0; column < columns; ++column)
    {
        const auto fraction0 = static_cast<double> (column) / static_cast<double> (columns);
        const auto fraction1 = static_cast<double> (column + 1) / static_cast<double> (columns);
        const auto edge0 = scale == FrequencyScale::linear
                               ? lower + fraction0 * (upper - lower)
                               : std::exp (logLower + fraction0 * (logUpper - logLower));
        const auto edge1 = scale == FrequencyScale::linear
                               ? lower + fraction1 * (upper - lower)
                               : std::exp (logLower + fraction1 * (logUpper - logLower));
        frequencies[column] = static_cast<float> (
            scale == FrequencyScale::linear ? 0.5 * (edge0 + edge1)
                                            : std::sqrt (edge0 * edge1));

        const auto firstBin = static_cast<std::size_t> (
            std::max (0.0, std::ceil (edge0 / binWidth - 0.5)));
        const auto lastBin = std::min (
            numBins - 1,
            static_cast<std::size_t> (std::max (0.0, std::floor (edge1 / binWidth + 0.5))));

        for (std::size_t channel = 0; channel < numChannels; ++channel)
        {
            double weightedPower = 0.0;
            double coveredWidth = 0.0;
            auto peak = -std::numeric_limits<float>::infinity();
            for (auto bin = firstBin; bin <= lastBin; ++bin)
            {
                const auto binLower = std::max (0.0, (static_cast<double> (bin) - 0.5) * binWidth);
                const auto binUpper = std::min (nyquist, (static_cast<double> (bin) + 0.5) * binWidth);
                const auto overlap = std::max (0.0, std::min (edge1, binUpper) - std::max (edge0, binLower));
                if (overlap <= 0.0)
                    continue;
                const auto power = planarPsd[channel * numBins + bin];
                if (! std::isfinite (power) || power < 0.0f)
                    continue;
                weightedPower += static_cast<double> (power) * overlap;
                coveredWidth += overlap;
                peak = std::max (peak, power);
            }

            const auto destination = channel * columns + column;
            means[destination] = coveredWidth > 0.0
                                     ? static_cast<float> (weightedPower / coveredWidth)
                                     : std::numeric_limits<float>::quiet_NaN();
            peaks[destination] = std::isfinite (peak)
                                     ? peak
                                     : std::numeric_limits<float>::quiet_NaN();
        }
    }

    view.frequenciesHz = frequencies.data();
    view.means = means.data();
    view.peaks = peaks.data();
    view.columnStride = columns;
    view.numChannels = numChannels;
    view.numColumns = columns;
    view.minimumFrequencyHz = lower;
    view.maximumFrequencyHz = upper;
    view.frequencyScale = scale;
    return true;
}
} // namespace spectrumviewer
