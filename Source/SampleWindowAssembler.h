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
#include <stdexcept>
#include <vector>

namespace spectrumviewer
{
/**
    Builds sample-indexed overlapping windows on the analysis thread.

    All history is private to the calling thread. Incoming blocks may have arbitrary
    sizes, but their first-sample positions must be contiguous. A gap or overlap resets
    partial history so a published window never splices discontinuous samples.
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

        /**
            Returns one channel in chronological order as at most two contiguous regions.

            The regions refer to the assembler's circular history and are valid only while
            the consumer passed to append() is running. The second region is empty when the
            window does not wrap around the end of the history allocation.
        */
        ChannelView getChannel (std::size_t channel) const noexcept
        {
            const auto* channelData = data + channel * numSamples;
            return { channelData + firstOffset,
                     numSamples - firstOffset,
                     channelData,
                     firstOffset };
        }

        std::uint64_t firstSample = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;

    private:
        friend class SampleWindowAssembler;
        const float* data = nullptr;
        std::size_t firstOffset = 0;
    };

    struct AppendResult
    {
        std::size_t windowsEmitted = 0;
        bool discontinuity = false;
        bool accepted = false;
    };

    SampleWindowAssembler (std::size_t numChannels,
                           std::size_t windowSize,
                           std::size_t hopSize)
        : channelCount (numChannels),
          windowSampleCount (windowSize),
          hopSampleCount (hopSize),
          history (numChannels * windowSize),
          samplesUntilWindow (windowSize)
    {
        if (numChannels == 0 || windowSize == 0 || hopSize == 0)
            throw std::invalid_argument ("SampleWindowAssembler dimensions must be non-zero");
    }

    template <typename Consumer>
    AppendResult append (const float* const* source,
                         std::size_t numChannels,
                         std::size_t numSamples,
                         std::uint64_t firstSample,
                         Consumer&& consumer)
    {
        AppendResult result;

        if (source == nullptr || numChannels != channelCount || numSamples == 0)
            return result;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            if (source[channel] == nullptr)
                return result;
        }

        result.accepted = true;

        if (! hasExpectedSample)
        {
            nextExpectedSample = firstSample;
            hasExpectedSample = true;
        }
        else if (firstSample != nextExpectedSample)
        {
            resetHistory (firstSample);
            ++discontinuityCount;
            result.discontinuity = true;
        }

        std::size_t sourceOffset = 0;

        while (sourceOffset < numSamples)
        {
            const auto samplesToCopy = std::min ({ numSamples - sourceOffset,
                                                   windowSampleCount - writePosition,
                                                   samplesUntilWindow });

            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                std::memcpy (history.data() + channel * windowSampleCount + writePosition,
                             source[channel] + sourceOffset,
                             samplesToCopy * sizeof (float));
            }

            sourceOffset += samplesToCopy;
            writePosition += samplesToCopy;
            if (writePosition == windowSampleCount)
                writePosition = 0;

            nextExpectedSample += samplesToCopy;
            samplesUntilWindow -= samplesToCopy;

            if (samplesUntilWindow == 0)
            {
                WindowView view;
                view.data = history.data();
                view.firstOffset = writePosition;
                view.firstSample = nextExpectedSample - windowSampleCount;
                view.numChannels = channelCount;
                view.numSamples = windowSampleCount;

                ++result.windowsEmitted;
                samplesUntilWindow = hopSampleCount;
                consumer (view);
            }
        }

        return result;
    }

    void reset() noexcept
    {
        writePosition = 0;
        samplesUntilWindow = windowSampleCount;
        nextExpectedSample = 0;
        hasExpectedSample = false;
    }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getWindowSize() const noexcept { return windowSampleCount; }
    std::size_t getHopSize() const noexcept { return hopSampleCount; }
    std::uint64_t getDiscontinuityCount() const noexcept { return discontinuityCount; }

private:
    void resetHistory (std::uint64_t firstSample) noexcept
    {
        writePosition = 0;
        samplesUntilWindow = windowSampleCount;
        nextExpectedSample = firstSample;
    }

    const std::size_t channelCount;
    const std::size_t windowSampleCount;
    const std::size_t hopSampleCount;
    std::vector<float> history;

    std::size_t writePosition = 0;
    std::size_t samplesUntilWindow;
    std::uint64_t nextExpectedSample = 0;
    std::uint64_t discontinuityCount = 0;
    bool hasExpectedSample = false;
};
} // namespace spectrumviewer

#endif // SAMPLE_WINDOW_ASSEMBLER_H_INCLUDED
