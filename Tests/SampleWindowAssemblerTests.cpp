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

#include "gtest/gtest.h"

#include "SampleWindowAssembler.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace
{
using spectrumviewer::SampleWindowAssembler;

struct CapturedWindow
{
    std::int64_t firstSample;
    std::vector<std::vector<float>> channels;
};

std::vector<std::vector<float>> copyChannels (const SampleWindowAssembler::WindowView& view)
{
    std::vector<std::vector<float>> result;

    for (std::size_t channel = 0; channel < view.numChannels; ++channel)
    {
        const auto channelView = view.getChannel (channel);
        auto& destination = result.emplace_back();
        destination.reserve (view.numSamples);
        destination.insert (destination.end(),
                            channelView.firstData,
                            channelView.firstData + channelView.firstSize);
        destination.insert (destination.end(),
                            channelView.secondData,
                            channelView.secondData + channelView.secondSize);
    }

    return result;
}

template <typename Consumer>
SampleWindowAssembler::AppendResult appendAndConsume (SampleWindowAssembler& assembler,
                                                       const float* const* source,
                                                       std::size_t numChannels,
                                                       std::size_t numSamples,
                                                       std::int64_t firstSample,
                                                       Consumer&& consumer)
{
    const auto result = assembler.appendBlock (source, numChannels, numSamples, firstSample);
    if (result.accepted)
    {
        EXPECT_EQ (assembler.consumeReadyWindows (std::forward<Consumer> (consumer)), result.windowsReady);
    }
    return result;
}

TEST (SampleWindowAssemblerTests, RejectsZeroSizedConfiguration)
{
    EXPECT_THROW ((SampleWindowAssembler { 0, 4, 2, 4 }), std::invalid_argument);
    EXPECT_THROW ((SampleWindowAssembler { 1, 0, 2, 4 }), std::invalid_argument);
    EXPECT_THROW ((SampleWindowAssembler { 1, 4, 0, 4 }), std::invalid_argument);
    EXPECT_THROW ((SampleWindowAssembler { 1, 4, 2, 0 }), std::invalid_argument);
}

TEST (SampleWindowAssemblerTests, ProducesExactOverlappingWindowsAcrossCallbacks)
{
    SampleWindowAssembler assembler (1, 4, 2, 4);
    const std::array<float, 10> samples { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f };
    const std::array<std::size_t, 4> callbackSizes { 1, 3, 2, 4 };
    std::vector<CapturedWindow> windows;
    std::size_t offset = 0;

    for (const auto callbackSize : callbackSizes)
    {
        const float* channels[] { samples.data() + offset };
        const auto result = appendAndConsume (assembler, channels, 1, callbackSize, static_cast<std::int64_t> (offset), [&] (const auto& window)
        {
            windows.push_back ({ window.firstSample, copyChannels (window) });
        });

        EXPECT_TRUE (result.accepted);
        EXPECT_FALSE (result.discontinuity);
        offset += callbackSize;
    }

    ASSERT_EQ (windows.size(), 4u);
    EXPECT_EQ (windows[0].firstSample, 0u);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 0.0f, 1.0f, 2.0f, 3.0f }));
    EXPECT_EQ (windows[1].firstSample, 2u);
    EXPECT_EQ (windows[1].channels[0], (std::vector<float> { 2.0f, 3.0f, 4.0f, 5.0f }));
    EXPECT_EQ (windows[2].firstSample, 4u);
    EXPECT_EQ (windows[2].channels[0], (std::vector<float> { 4.0f, 5.0f, 6.0f, 7.0f }));
    EXPECT_EQ (windows[3].firstSample, 6u);
    EXPECT_EQ (windows[3].channels[0], (std::vector<float> { 6.0f, 7.0f, 8.0f, 9.0f }));
}

TEST (SampleWindowAssemblerTests, CopiesACompleteBlockBeforeWindowsAreConsumed)
{
    SampleWindowAssembler assembler (1, 4, 2, 6);
    std::array<float, 6> samples { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    const float* channels[] { samples.data() };

    const auto result = assembler.appendBlock (channels, 1, samples.size(), 100);
    ASSERT_TRUE (result.accepted);
    ASSERT_EQ (result.windowsReady, 2u);

    samples.fill (-1.0f); // Model the producer reusing a released FIFO slot.
    std::vector<CapturedWindow> windows;
    EXPECT_EQ (assembler.consumeReadyWindows ([&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    }), 2u);

    ASSERT_EQ (windows.size(), 2u);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 0.0f, 1.0f, 2.0f, 3.0f }));
    EXPECT_EQ (windows[1].channels[0], (std::vector<float> { 2.0f, 3.0f, 4.0f, 5.0f }));
}

TEST (SampleWindowAssemblerTests, RequiresReadyWindowsToBeConsumedBeforeAnotherAppend)
{
    SampleWindowAssembler assembler (1, 4, 2, 4);
    const std::array<float, 4> first { 0.0f, 1.0f, 2.0f, 3.0f };
    const std::array<float, 2> second { 4.0f, 5.0f };
    const float* firstChannel[] { first.data() };
    const float* secondChannel[] { second.data() };

    ASSERT_TRUE (assembler.appendBlock (firstChannel, 1, first.size(), 0).accepted);
    EXPECT_FALSE (assembler.appendBlock (secondChannel, 1, second.size(), 4).accepted);
    EXPECT_EQ (assembler.consumeReadyWindows ([] (const auto&) {}), 1u);
    EXPECT_TRUE (assembler.appendBlock (secondChannel, 1, second.size(), 4).accepted);
}

TEST (SampleWindowAssemblerTests, DiscardsOldWindowsWithoutChangingTheHopGrid)
{
    SampleWindowAssembler assembler (1, 4, 2, 6);
    const std::array<float, 8> samples { 0, 1, 2, 3, 4, 5, 6, 7 };
    const float* first[] { samples.data() };

    const auto append = assembler.appendBlock (first, 1, 6, 100);
    ASSERT_TRUE (append.accepted);
    ASSERT_EQ (append.windowsReady, 2u);
    EXPECT_EQ (assembler.discardReadyWindows (1), 1u);

    std::vector<CapturedWindow> windows;
    EXPECT_EQ (assembler.consumeReadyWindows ([&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    }), 1u);
    ASSERT_EQ (windows.size(), 1u);
    EXPECT_EQ (windows[0].firstSample, 102);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 2, 3, 4, 5 }));

    const float* second[] { samples.data() + 6 };
    const auto next = assembler.appendBlock (second, 1, 2, 106);
    ASSERT_TRUE (next.accepted);
    ASSERT_EQ (next.windowsReady, 1u);
    EXPECT_EQ (assembler.discardReadyWindows(), 1u);
    EXPECT_FALSE (assembler.hasReadyWindow());
}

TEST (SampleWindowAssemblerTests, ExposesWrappedWindowsAsTwoChronologicalRegions)
{
    SampleWindowAssembler assembler (1, 4, 2, 4);
    const std::array<float, 10> samples { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f };
    std::vector<std::pair<std::size_t, std::size_t>> regionSizes;
    std::vector<CapturedWindow> windows;

    for (std::size_t offset = 0; offset < samples.size(); offset += 2)
    {
        const float* channels[] { samples.data() + offset };
        appendAndConsume (assembler, channels, 1, 2, static_cast<std::int64_t> (offset), [&] (const auto& window)
        {
            const auto channel = window.getChannel (0);
            regionSizes.emplace_back (channel.firstSize, channel.secondSize);
            windows.push_back ({ window.firstSample, copyChannels (window) });
        });
    }

    ASSERT_FALSE (regionSizes.empty());
    EXPECT_EQ (regionSizes.back(), (std::pair<std::size_t, std::size_t> { 2, 2 }));
    ASSERT_EQ (windows.size(), 4u);
    EXPECT_EQ (windows.back().channels[0], (std::vector<float> { 6.0f, 7.0f, 8.0f, 9.0f }));
}

TEST (SampleWindowAssemblerTests, CallbackPartitioningDoesNotChangeWindows)
{
    constexpr std::size_t numChannels = 3;
    constexpr std::size_t totalSamples = 997;
    std::array<std::vector<float>, numChannels> samples;

    for (std::size_t channel = 0; channel < numChannels; ++channel)
    {
        samples[channel].resize (totalSamples);
        for (std::size_t sample = 0; sample < totalSamples; ++sample)
            samples[channel][sample] = static_cast<float> (channel * 10000 + sample);
    }

    auto assemble = [&] (std::size_t windowSize,
                          std::size_t hopSize,
                          const std::vector<std::size_t>& callbackSizes)
    {
        SampleWindowAssembler assembler (numChannels, windowSize, hopSize, totalSamples);
        std::vector<CapturedWindow> windows;
        std::size_t offset = 0;

        for (const auto callbackSize : callbackSizes)
        {
            std::array<const float*, numChannels> channels;
            for (std::size_t channel = 0; channel < numChannels; ++channel)
                channels[channel] = samples[channel].data() + offset;

            const auto result = appendAndConsume (assembler,
                                                  channels.data(),
                                                  numChannels,
                                                  callbackSize,
                                                  5000 + static_cast<std::int64_t> (offset),
                                                  [&] (const auto& window)
            {
                windows.push_back ({ window.firstSample, copyChannels (window) });
            });

            EXPECT_TRUE (result.accepted);
            EXPECT_FALSE (result.discontinuity);
            offset += callbackSize;
        }

        EXPECT_EQ (offset, totalSamples);
        return windows;
    };

    std::mt19937 generator (12345);
    std::uniform_int_distribution<std::size_t> blockSize (1, 53);
    std::vector<std::size_t> randomCallbackSizes;
    std::size_t remaining = totalSamples;

    while (remaining > 0)
    {
        const auto callbackSize = std::min (remaining, blockSize (generator));
        randomCallbackSizes.push_back (callbackSize);
        remaining -= callbackSize;
    }

    const std::array<std::pair<std::size_t, std::size_t>, 5> layouts {
        std::pair<std::size_t, std::size_t> { 1, 1 },
        std::pair<std::size_t, std::size_t> { 8, 3 },
        std::pair<std::size_t, std::size_t> { 31, 7 },
        std::pair<std::size_t, std::size_t> { 64, 32 },
        std::pair<std::size_t, std::size_t> { 127, 127 }
    };

    for (const auto [windowSize, hopSize] : layouts)
    {
        const auto reference = assemble (windowSize, hopSize, { totalSamples });
        const auto partitioned = assemble (windowSize, hopSize, randomCallbackSizes);
        ASSERT_EQ (partitioned.size(), reference.size());

        for (std::size_t index = 0; index < reference.size(); ++index)
        {
            EXPECT_EQ (partitioned[index].firstSample, reference[index].firstSample);
            EXPECT_EQ (partitioned[index].channels, reference[index].channels);
        }
    }
}

TEST (SampleWindowAssemblerTests, BoundedConsumptionLeavesLaterWindowsReady)
{
    SampleWindowAssembler assembler (1, 4, 2, 8);
    const std::array<float, 8> samples { 0, 1, 2, 3, 4, 5, 6, 7 };
    const float* channels[] { samples.data() };
    const auto appended = assembler.appendBlock (channels, 1, samples.size(), 0);
    ASSERT_TRUE (appended.accepted);
    EXPECT_EQ (appended.windowsReady, 3u);

    std::vector<std::int64_t> positions;
    EXPECT_EQ (assembler.consumeReadyWindows (
                   [&positions] (const auto& window)
                   { positions.push_back (window.firstSample); },
                   1),
               1u);
    EXPECT_EQ (positions, (std::vector<std::int64_t> { 0 }));
    EXPECT_EQ (assembler.consumeReadyWindows (
                   [&positions] (const auto& window)
                   { positions.push_back (window.firstSample); }),
               2u);
    EXPECT_EQ (positions, (std::vector<std::int64_t> { 0, 2, 4 }));
}

TEST (SampleWindowAssemblerTests, DiscontinuityDiscardsPartialWindow)
{
    SampleWindowAssembler assembler (1, 4, 2, 6);
    const std::array<float, 3> partial { 0.0f, 1.0f, 2.0f };
    const std::array<float, 6> afterGap { 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f };
    std::vector<CapturedWindow> windows;

    const float* partialChannel[] { partial.data() };
    const auto first = appendAndConsume (assembler, partialChannel, 1, partial.size(), 0, [&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    });

    EXPECT_FALSE (first.discontinuity);
    EXPECT_EQ (first.windowsReady, 0u);

    const float* channelAfterGap[] { afterGap.data() };
    const auto second = appendAndConsume (assembler, channelAfterGap, 1, afterGap.size(), 10, [&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    });

    EXPECT_TRUE (second.discontinuity);
    EXPECT_EQ (assembler.getDiscontinuityCount(), 1u);
    ASSERT_EQ (windows.size(), 2u);
    EXPECT_EQ (windows[0].firstSample, 10u);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 10.0f, 11.0f, 12.0f, 13.0f }));
    EXPECT_EQ (windows[1].firstSample, 12u);
    EXPECT_EQ (windows[1].channels[0], (std::vector<float> { 12.0f, 13.0f, 14.0f, 15.0f }));
}

TEST (SampleWindowAssemblerTests, OverlappingInputDiscardsPartialWindow)
{
    SampleWindowAssembler assembler (1, 4, 2, 4);
    const std::array<float, 3> partial { 0.0f, 1.0f, 2.0f };
    const std::array<float, 4> replayed { 1.0f, 2.0f, 3.0f, 4.0f };
    std::vector<CapturedWindow> windows;

    const float* partialChannel[] { partial.data() };
    appendAndConsume (assembler, partialChannel, 1, partial.size(), 0, [&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    });

    const float* replayedChannel[] { replayed.data() };
    const auto result = appendAndConsume (assembler, replayedChannel, 1, replayed.size(), 1, [&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    });

    EXPECT_TRUE (result.discontinuity);
    EXPECT_EQ (result.windowsReady, 1u);
    EXPECT_EQ (assembler.getDiscontinuityCount(), 1u);
    ASSERT_EQ (windows.size(), 1u);
    EXPECT_EQ (windows[0].firstSample, 1u);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 1.0f, 2.0f, 3.0f, 4.0f }));
}

TEST (SampleWindowAssemblerTests, KeepsChannelSamplesPlanar)
{
    SampleWindowAssembler assembler (2, 3, 3, 3);
    const std::array<float, 3> channel0 { 1.0f, 2.0f, 3.0f };
    const std::array<float, 3> channel1 { 11.0f, 12.0f, 13.0f };
    const float* channels[] { channel0.data(), channel1.data() };
    std::vector<std::vector<float>> captured;

    const auto result = appendAndConsume (assembler, channels, 2, 3, 50, [&] (const auto& window)
    {
        captured = copyChannels (window);
    });

    EXPECT_TRUE (result.accepted);
    EXPECT_EQ (result.windowsReady, 1u);
    ASSERT_EQ (captured.size(), 2u);
    EXPECT_EQ (captured[0], (std::vector<float> { 1.0f, 2.0f, 3.0f }));
    EXPECT_EQ (captured[1], (std::vector<float> { 11.0f, 12.0f, 13.0f }));
}
} // namespace
