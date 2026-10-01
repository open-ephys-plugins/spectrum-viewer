/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "SpectrumReference.h"

#include "SpectrumSupport.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace spectrumviewer
{
bool computeDecibelDelta (const float* currentPsd,
                          const float* referencePsd,
                          float* destination,
                          std::size_t valueCount) noexcept
{
    if (currentPsd == nullptr || referencePsd == nullptr
        || destination == nullptr || valueCount == 0)
        return false;

    for (std::size_t index = 0; index < valueCount; ++index)
    {
        const auto current = currentPsd[index];
        const auto reference = referencePsd[index];
        destination[index] = std::isfinite (current) && current > 0.0f
                                     && std::isfinite (reference) && reference > 0.0f
                                 ? 10.0f * (std::log10 (current)
                                            - std::log10 (reference))
                                 : std::numeric_limits<float>::quiet_NaN();
    }
    return true;
}

CapturedSpectrum::CapturedSpectrum (
    std::uint64_t captureId,
    std::int64_t capturedAtUnixMilliseconds,
    SpectrumFrameDescriptor descriptor,
    std::vector<int> sourceChannelIndices,
    std::vector<std::string> sourceChannelUnits,
    std::vector<float> planarMeanPsd,
    std::vector<float> planarSampleVariance,
    SpectrumCaptureQuality quality,
    std::optional<std::uint16_t> sourceStreamId,
    SpectrumSourceLabels sourceLabels)
    : identifier (captureId),
      capturedAtMilliseconds (capturedAtUnixMilliseconds),
      frameDescriptor (descriptor),
      channelIndices (std::move (sourceChannelIndices)),
      channelUnits (std::move (sourceChannelUnits)),
      meanPsd (std::move (planarMeanPsd)),
      sampleVariance (std::move (planarSampleVariance)),
      captureQuality (quality),
      streamId (sourceStreamId),
      labels (std::move (sourceLabels))
{
    const auto bins = descriptor.windowSampleCount / 2 + 1;
    const auto valueCount = support::checkedPositiveProduct (
        { channelIndices.size(), bins }, "Captured spectrum dimensions are invalid");
    if (captureId == 0 || capturedAtUnixMilliseconds <= 0
        || channelUnits.size() != channelIndices.size()
        || meanPsd.size() != valueCount || sampleVariance.size() != valueCount
        || ! std::isfinite (descriptor.sampleRateHz) || descriptor.sampleRateHz <= 0.0
        || descriptor.windowSampleCount == 0 || descriptor.taperCount == 0
        || ! std::isfinite (descriptor.timeHalfBandwidth)
        || descriptor.timeHalfBandwidth <= 0.0
        || quality.includedWindowCount == 0
        || quality.includedWindowCount != quality.targetWindowCount
        || quality.lastSampleExclusive <= quality.firstSample
        || (! labels.channelNames.empty() && labels.channelNames.size() != channelIndices.size())
        || (isBaseline() && channelIndices.size() != 1))
        throw std::invalid_argument ("Captured spectrum metadata is invalid");

    for (std::size_t index = 0; index < valueCount; ++index)
        if (! std::isfinite (meanPsd[index]) || meanPsd[index] < 0.0f
            || ! std::isfinite (sampleVariance[index]) || sampleVariance[index] < 0.0f)
            throw std::invalid_argument ("Captured spectrum values are invalid");
}

SpectrumReferenceMismatch CapturedSpectrum::findMismatch (
    const SpectrumFrameDescriptor& candidate,
    const std::vector<int>& candidateChannelIndices,
    const std::vector<std::string>& candidateChannelUnits,
    std::uint16_t candidateStreamId) const noexcept
{
    if (streamId.has_value() && candidateStreamId != *streamId)
        return SpectrumReferenceMismatch::stream;
    if (candidate.sampleRateHz != frameDescriptor.sampleRateHz)
        return SpectrumReferenceMismatch::sampleRate;

    // Hop and configuration generation describe scheduling, not the spectral
    // estimator. Capture windows may therefore be compared with overlapping
    // live Fine windows made by the same estimator.
    if (candidate.binWidthHz != frameDescriptor.binWidthHz
        || candidate.timeHalfBandwidth != frameDescriptor.timeHalfBandwidth
        || candidate.windowSampleCount != frameDescriptor.windowSampleCount
        || candidate.taperCount != frameDescriptor.taperCount
        || candidate.detrendMode != frameDescriptor.detrendMode
        || candidate.valueKind != frameDescriptor.valueKind)
        return SpectrumReferenceMismatch::estimator;

    // A baseline describes what one channel looks like under ideal conditions,
    // to hold any channel up against, so which channels are selected and what
    // they are called does not matter.
    if (isBaseline())
        return SpectrumReferenceMismatch::none;
    if (candidateChannelIndices != channelIndices)
        return SpectrumReferenceMismatch::channels;
    if (candidateChannelUnits != channelUnits)
        return SpectrumReferenceMismatch::units;
    return SpectrumReferenceMismatch::none;
}
} // namespace spectrumviewer
