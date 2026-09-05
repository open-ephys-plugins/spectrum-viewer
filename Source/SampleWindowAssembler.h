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
        const float* getChannelData (std::size_t channel) const noexcept
        {
            return data + channel * numSamples;
        }

        std::uint64_t firstSample = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;

    private:
        friend class SampleWindowAssembler;
        const float* data = nullptr;
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
          contiguousWindow (numChannels * windowSize),
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

        for (std::size_t sample = 0; sample < numSamples; ++sample)
        {
            for (std::size_t channel = 0; channel < channelCount; ++channel)
                history[channel * windowSampleCount + writePosition] = source[channel][sample];

            writePosition = increment (writePosition);
            ++nextExpectedSample;
            --samplesUntilWindow;

            if (samplesUntilWindow == 0)
            {
                makeContiguousWindow();

                WindowView view;
                view.data = contiguousWindow.data();
                view.firstSample = nextExpectedSample - windowSampleCount;
                view.numChannels = channelCount;
                view.numSamples = windowSampleCount;
                consumer (view);

                ++result.windowsEmitted;
                samplesUntilWindow = hopSampleCount;
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
    std::size_t increment (std::size_t index) const noexcept
    {
        ++index;
        return index == windowSampleCount ? 0 : index;
    }

    void resetHistory (std::uint64_t firstSample) noexcept
    {
        writePosition = 0;
        samplesUntilWindow = windowSampleCount;
        nextExpectedSample = firstSample;
    }

    void makeContiguousWindow() noexcept
    {
        const auto firstPart = windowSampleCount - writePosition;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            const auto* source = history.data() + channel * windowSampleCount;
            auto* destination = contiguousWindow.data() + channel * windowSampleCount;

            std::memcpy (destination,
                         source + writePosition,
                         firstPart * sizeof (float));
            std::memcpy (destination + firstPart,
                         source,
                         writePosition * sizeof (float));
        }
    }

    const std::size_t channelCount;
    const std::size_t windowSampleCount;
    const std::size_t hopSampleCount;
    std::vector<float> history;
    std::vector<float> contiguousWindow;

    std::size_t writePosition = 0;
    std::size_t samplesUntilWindow;
    std::uint64_t nextExpectedSample = 0;
    std::uint64_t discontinuityCount = 0;
    bool hasExpectedSample = false;
};
} // namespace spectrumviewer

#endif // SAMPLE_WINDOW_ASSEMBLER_H_INCLUDED
