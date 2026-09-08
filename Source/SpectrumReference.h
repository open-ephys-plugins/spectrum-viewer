/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef SPECTRUM_REFERENCE_H_INCLUDED
#define SPECTRUM_REFERENCE_H_INCLUDED

#include "SpectrumEstimation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spectrumviewer
{
enum class SpectrumComparisonMode
{
    absolute = 1,
    overlay = 2,
    deltaDb = 3
};

enum class SpectrumReferenceCompatibility
{
    noReference,
    compatible,
    incompatible
};

/** Computes 10*log10(current/reference), preserving invalid bins as NaN. */
bool computeDecibelDelta (const float* currentPsd,
                          const float* referencePsd,
                          float* destination,
                          std::size_t valueCount) noexcept;

struct SpectrumCaptureQuality
{
    std::size_t includedWindowCount = 0;
    std::size_t targetWindowCount = 0;
    std::int64_t firstSample = 0;
    std::int64_t lastSampleExclusive = 0;
    std::uint64_t failedWindowCount = 0;
    std::uint64_t shedWindowCount = 0;
    std::uint64_t discontinuityCount = 0;
};

/** Immutable, full-resolution result of one completed spectrum capture. */
class CapturedSpectrum
{
public:
    CapturedSpectrum (std::uint64_t captureId,
                      std::int64_t capturedAtUnixMilliseconds,
                      SpectrumFrameDescriptor descriptor,
                      std::vector<int> sourceChannelIndices,
                      std::vector<std::string> sourceChannelUnits,
                      std::vector<float> planarMeanPsd,
                      std::vector<float> planarSampleVariance,
                      SpectrumCaptureQuality quality);

    std::uint64_t getCaptureId() const noexcept { return identifier; }
    std::int64_t getCapturedAtUnixMilliseconds() const noexcept { return capturedAtMilliseconds; }
    const SpectrumFrameDescriptor& getDescriptor() const noexcept { return frameDescriptor; }
    const std::vector<int>& getSourceChannelIndices() const noexcept { return channelIndices; }
    const std::vector<std::string>& getSourceChannelUnits() const noexcept { return channelUnits; }
    const float* getPlanarMeanPsd() const noexcept { return meanPsd.data(); }
    const float* getPlanarSampleVariance() const noexcept { return sampleVariance.data(); }
    std::size_t getChannelCount() const noexcept { return channelIndices.size(); }
    std::size_t getBinCount() const noexcept { return frameDescriptor.windowSampleCount / 2 + 1; }
    const SpectrumCaptureQuality& getQuality() const noexcept { return captureQuality; }

    bool isCompatibleWith (const SpectrumFrameDescriptor& candidateDescriptor,
                           const std::vector<int>& candidateChannelIndices,
                           const std::vector<std::string>& candidateChannelUnits) const noexcept;

private:
    std::uint64_t identifier;
    std::int64_t capturedAtMilliseconds;
    SpectrumFrameDescriptor frameDescriptor;
    std::vector<int> channelIndices;
    std::vector<std::string> channelUnits;
    std::vector<float> meanPsd;
    std::vector<float> sampleVariance;
    SpectrumCaptureQuality captureQuality;
};
} // namespace spectrumviewer

#endif // SPECTRUM_REFERENCE_H_INCLUDED
