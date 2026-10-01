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
#include <optional>
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
    incompatible,
    // A reference is held but has not been compared with live analysis yet:
    // it was just imported, or acquisition is stopped. Frames never carry
    // this; it exists for what the controls report between runs.
    unchecked
};

/** The first reason a reference cannot be compared with live data, in the
    order they are checked. A baseline is only ever a sampleRate or estimator
    mismatch. */
enum class SpectrumReferenceMismatch
{
    none,
    stream,
    sampleRate,
    estimator,
    channels,
    units
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

/** Human-readable names for where a spectrum came from. Informational only:
    compatibility never depends on them, because names are not unique. */
struct SpectrumSourceLabels
{
    std::string streamName;
    std::vector<std::string> channelNames;

    bool empty() const noexcept { return streamName.empty() && channelNames.empty(); }
};

/** Immutable, full-resolution result of one completed spectrum capture.

    It is one of two kinds. A capture made this session is bound to the stream
    and channels it was recorded on, and compared channel by channel. A
    baseline is a single channel read from a file: it is compared with every
    selected channel of whichever stream is selected, and only the sample rate
    and the estimator have to match. */
class CapturedSpectrum
{
public:
    /** sourceStreamId binds the capture to one stream. Pass std::nullopt to
        make a baseline, which must have exactly one channel: stream ids are
        assigned per session, so the one it was recorded on means nothing now. */
    CapturedSpectrum (std::uint64_t captureId,
                      std::int64_t capturedAtUnixMilliseconds,
                      SpectrumFrameDescriptor descriptor,
                      std::vector<int> sourceChannelIndices,
                      std::vector<std::string> sourceChannelUnits,
                      std::vector<float> planarMeanPsd,
                      std::vector<float> planarSampleVariance,
                      SpectrumCaptureQuality quality,
                      std::optional<std::uint16_t> sourceStreamId = std::uint16_t { 0 },
                      SpectrumSourceLabels sourceLabels = {});

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
    /** The stream this capture is bound to, or zero for a baseline. */
    std::uint16_t getSourceStreamId() const noexcept { return streamId.value_or (0); }
    /** True for a single channel read from a file; see the class comment. */
    bool isBaseline() const noexcept { return ! streamId.has_value(); }
    const SpectrumSourceLabels& getSourceLabels() const noexcept { return labels; }

    /** Returns why live data from the candidate configuration cannot be
        compared with this capture, or none if it can. A baseline ignores the
        candidate's stream, channels and units. */
    SpectrumReferenceMismatch findMismatch (const SpectrumFrameDescriptor& candidateDescriptor,
                                            const std::vector<int>& candidateChannelIndices,
                                            const std::vector<std::string>& candidateChannelUnits,
                                            std::uint16_t candidateStreamId = 0) const noexcept;

    bool isCompatibleWith (const SpectrumFrameDescriptor& candidateDescriptor,
                           const std::vector<int>& candidateChannelIndices,
                           const std::vector<std::string>& candidateChannelUnits,
                           std::uint16_t candidateStreamId = 0) const noexcept
    {
        return findMismatch (candidateDescriptor, candidateChannelIndices,
                             candidateChannelUnits, candidateStreamId)
               == SpectrumReferenceMismatch::none;
    }

private:
    std::uint64_t identifier;
    std::int64_t capturedAtMilliseconds;
    SpectrumFrameDescriptor frameDescriptor;
    std::vector<int> channelIndices;
    std::vector<std::string> channelUnits;
    std::vector<float> meanPsd;
    std::vector<float> sampleVariance;
    SpectrumCaptureQuality captureQuality;
    std::optional<std::uint16_t> streamId;
    SpectrumSourceLabels labels;
};
} // namespace spectrumviewer

#endif // SPECTRUM_REFERENCE_H_INCLUDED
