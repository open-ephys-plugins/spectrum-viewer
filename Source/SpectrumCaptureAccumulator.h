/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef SPECTRUM_CAPTURE_ACCUMULATOR_H_INCLUDED
#define SPECTRUM_CAPTURE_ACCUMULATOR_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

namespace spectrumviewer
{
/**
    Worker-owned online mean and variance for equally weighted planar PSDs.

    Storage is allocated only at construction. add() validates a complete input
    before changing either accumulator, so a rejected spectrum cannot partially
    contaminate a capture.
*/
class SpectrumCaptureAccumulator
{
public:
    SpectrumCaptureAccumulator (std::size_t channelCount,
                                std::size_t binCount,
                                std::size_t targetWindowCount);

    bool add (const float* planarPsd,
              std::size_t channelCount,
              std::size_t binCount) noexcept;
    void reset() noexcept;

    const float* getChannelMean (std::size_t channel) const noexcept;
    float getSampleVariance (std::size_t channel, std::size_t bin) const noexcept;
    const float* getPlanarMean() const noexcept { return means.data(); }

    std::size_t getChannelCount() const noexcept { return numChannels; }
    std::size_t getBinCount() const noexcept { return numBins; }
    std::size_t getTargetWindowCount() const noexcept { return targetWindows; }
    std::size_t getIncludedWindowCount() const noexcept { return includedWindows; }
    std::uint64_t getRejectedWindowCount() const noexcept { return rejectedWindows; }
    bool isComplete() const noexcept { return includedWindows >= targetWindows; }

private:
    static std::size_t checkedValueCount (std::size_t channelCount,
                                          std::size_t binCount);

    const std::size_t numChannels;
    const std::size_t numBins;
    const std::size_t targetWindows;
    std::size_t includedWindows = 0;
    std::uint64_t rejectedWindows = 0;
    std::vector<float> means;
    std::vector<float> sumSquaredDifferences;
};
} // namespace spectrumviewer

#endif // SPECTRUM_CAPTURE_ACCUMULATOR_H_INCLUDED
