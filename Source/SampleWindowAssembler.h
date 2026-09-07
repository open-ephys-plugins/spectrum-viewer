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

#ifndef SAMPLE_WINDOW_ASSEMBLER_H_INCLUDED
#define SAMPLE_WINDOW_ASSEMBLER_H_INCLUDED

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace spectrumviewer
{
/**
    Builds sample-indexed overlapping windows on the analysis thread.

    appendBlock() copies a complete incoming block into private circular history.
    Ready windows must subsequently be consumed or explicitly discarded. Keeping
    these phases separate allows an input FIFO slot to be released before any DSP.

    Callers must consume all ready windows before appending another block. History
    holds one analysis window plus one maximum-sized input block, so every window
    completed by the most recent append remains valid without another sample copy.
*/
class SampleWindowAssembler
{
public:
    class WindowView
    {
    public:
        struct ChannelView
        {
            const float* firstData = nullptr;
            std::size_t firstSize = 0;
            const float* secondData = nullptr;
            std::size_t secondSize = 0;
        };

        /** Returns one channel chronologically as at most two circular regions. */
        ChannelView getChannel (std::size_t channel) const noexcept
        {
            const auto* channelData = data + channel * channelStride;
            const auto firstSize = std::min (numSamples, channelStride - firstOffset);
            return { channelData + firstOffset,
                     firstSize,
                     channelData,
                     numSamples - firstSize };
        }

        std::int64_t firstSample = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;

    private:
        friend class SampleWindowAssembler;
        const float* data = nullptr;
        std::size_t channelStride = 0;
        std::size_t firstOffset = 0;
    };

    struct AppendResult
    {
        std::size_t windowsReady = 0;
        bool discontinuity = false;
        bool accepted = false;
    };

    SampleWindowAssembler (std::size_t numChannels,
                           std::size_t windowSize,
                           std::size_t hopSize,
                           std::size_t maxSamplesPerBlock)
        : channelCount (numChannels),
          windowSampleCount (windowSize),
          hopSampleCount (hopSize),
          maximumAppendSampleCount (maxSamplesPerBlock),
          historySampleCount (checkedHistorySampleCount (windowSize, maxSamplesPerBlock)),
          history (checkedStorageSampleCount (numChannels, historySampleCount))
    {
        if (numChannels == 0 || windowSize == 0 || hopSize == 0 || maxSamplesPerBlock == 0)
            throw std::invalid_argument ("SampleWindowAssembler dimensions must be non-zero");
        if (hopSize > static_cast<std::size_t> (std::numeric_limits<std::int64_t>::max()))
            throw std::length_error ("SampleWindowAssembler hop size is out of range");
    }

    AppendResult appendBlock (const float* const* source,
                              std::size_t numChannels,
                              std::size_t numSamples,
                              std::int64_t firstSample)
    {
        AppendResult result;

        if (source == nullptr || numChannels != channelCount || numSamples == 0
            || numSamples > maximumAppendSampleCount || hasReadyWindow())
            return result;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
            if (source[channel] == nullptr)
                return result;

        result.accepted = true;

        if (! hasExpectedSample)
        {
            resetHistory (firstSample);
            hasExpectedSample = true;
        }
        else if (firstSample != nextExpectedSample)
        {
            resetHistory (firstSample);
            ++discontinuityCount;
            result.discontinuity = true;
        }

        const auto firstCopySize = std::min (numSamples, historySampleCount - writePosition);
        const auto secondCopySize = numSamples - firstCopySize;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto* channelHistory = history.data() + channel * historySampleCount;
            std::memcpy (channelHistory + writePosition,
                         source[channel],
                         firstCopySize * sizeof (float));
            if (secondCopySize > 0)
                std::memcpy (channelHistory,
                             source[channel] + firstCopySize,
                             secondCopySize * sizeof (float));
        }

        writePosition = (writePosition + numSamples) % historySampleCount;
        nextExpectedSample += static_cast<std::int64_t> (numSamples);
        result.windowsReady = getNumReadyWindows();
        return result;
    }

    template <typename Consumer>
    std::size_t consumeReadyWindows (Consumer&& consumer)
    {
        std::size_t consumed = 0;
        while (hasReadyWindow())
        {
            const auto samplesBehindWrite = static_cast<std::size_t> (
                nextExpectedSample - nextWindowFirstSample);

            WindowView view;
            view.data = history.data();
            view.channelStride = historySampleCount;
            view.firstOffset = (writePosition + historySampleCount
                                - samplesBehindWrite % historySampleCount)
                % historySampleCount;
            view.firstSample = nextWindowFirstSample;
            view.numChannels = channelCount;
            view.numSamples = windowSampleCount;

            consumer (view);
            nextWindowFirstSample += static_cast<std::int64_t> (hopSampleCount);
            ++consumed;
        }
        return consumed;
    }

    /**
        Advances past the oldest ready windows without exposing their samples.

        Discarding changes estimator cadence, but it does not discard input
        samples, splice history, or change the sample-indexed hop grid.
    */
    std::size_t discardReadyWindows (std::size_t windowsToKeep = 0) noexcept
    {
        const auto ready = getNumReadyWindows();
        const auto discarded = ready > windowsToKeep ? ready - windowsToKeep : 0;
        nextWindowFirstSample += static_cast<std::int64_t> (discarded * hopSampleCount);
        return discarded;
    }

    bool hasReadyWindow() const noexcept
    {
        return hasExpectedSample
            && nextExpectedSample - nextWindowFirstSample
                >= static_cast<std::int64_t> (windowSampleCount);
    }

    void reset() noexcept
    {
        writePosition = 0;
        nextExpectedSample = 0;
        nextWindowFirstSample = 0;
        hasExpectedSample = false;
    }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getWindowSize() const noexcept { return windowSampleCount; }
    std::size_t getHopSize() const noexcept { return hopSampleCount; }
    std::size_t getMaxSamplesPerBlock() const noexcept { return maximumAppendSampleCount; }
    std::size_t getHistorySize() const noexcept { return historySampleCount; }
    std::size_t getBufferedSampleCount() const noexcept
    {
        if (! hasExpectedSample)
            return 0;
        const auto available = static_cast<std::size_t> (
            nextExpectedSample - nextWindowFirstSample);
        return std::min (available, windowSampleCount);
    }
    std::uint64_t getDiscontinuityCount() const noexcept { return discontinuityCount; }

private:
    static std::size_t checkedHistorySampleCount (std::size_t windowSize,
                                                  std::size_t maxSamplesPerBlock)
    {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        if (windowSize == 0 || maxSamplesPerBlock == 0)
            return 0;
        if (windowSize > maximum - maxSamplesPerBlock)
            throw std::length_error ("SampleWindowAssembler history is too large");
        if (windowSize > static_cast<std::size_t> (std::numeric_limits<std::int64_t>::max())
            || maxSamplesPerBlock > static_cast<std::size_t> (std::numeric_limits<std::int64_t>::max()))
            throw std::length_error ("SampleWindowAssembler sample count is out of range");
        return windowSize + maxSamplesPerBlock;
    }

    static std::size_t checkedStorageSampleCount (std::size_t numChannels,
                                                  std::size_t historySize)
    {
        if (numChannels == 0 || historySize == 0)
            return 0;
        if (numChannels > std::numeric_limits<std::size_t>::max() / historySize)
            throw std::length_error ("SampleWindowAssembler allocation is too large");
        return numChannels * historySize;
    }

    std::size_t getNumReadyWindows() const noexcept
    {
        if (! hasReadyWindow())
            return 0;

        const auto samplesPastFirstWindow = static_cast<std::size_t> (
            nextExpectedSample - nextWindowFirstSample
            - static_cast<std::int64_t> (windowSampleCount));
        return 1 + samplesPastFirstWindow / hopSampleCount;
    }

    void resetHistory (std::int64_t firstSample) noexcept
    {
        writePosition = 0;
        nextExpectedSample = firstSample;
        nextWindowFirstSample = firstSample;
    }

    const std::size_t channelCount;
    const std::size_t windowSampleCount;
    const std::size_t hopSampleCount;
    const std::size_t maximumAppendSampleCount;
    const std::size_t historySampleCount;
    std::vector<float> history;

    std::size_t writePosition = 0;
    std::int64_t nextExpectedSample = 0;
    std::int64_t nextWindowFirstSample = 0;
    std::uint64_t discontinuityCount = 0;
    bool hasExpectedSample = false;
};
} // namespace spectrumviewer

#endif // SAMPLE_WINDOW_ASSEMBLER_H_INCLUDED
