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

#include "SampleBlockFifo.h"
#include "SampleWindowAssembler.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
using spectrumviewer::SampleBlockFifo;
using spectrumviewer::SampleWindowAssembler;

TEST (SampleBlockFifoTests, RejectsInvalidConfiguration)
{
    EXPECT_THROW ((SampleBlockFifo { 0, 4, 2 }), std::invalid_argument);
    EXPECT_THROW ((SampleBlockFifo { 1, 0, 2 }), std::invalid_argument);
    EXPECT_THROW ((SampleBlockFifo { 1, 4, 0 }), std::invalid_argument);
}

TEST (SampleBlockFifoTests, PreservesPlanarSamplesAndMetadata)
{
    SampleBlockFifo fifo (2, 4, 2);
    const std::array<float, 3> channel0 { 1.0f, 2.0f, 3.0f };
    const std::array<float, 3> channel1 { 11.0f, 12.0f, 13.0f };
    const float* channels[] { channel0.data(), channel1.data() };

    ASSERT_TRUE (fifo.tryPush (channels, 2, 3, 42, 7));
    ASSERT_EQ (fifo.getNumReady(), 1u);

    ASSERT_TRUE (fifo.tryPop ([&] (const SampleBlockFifo::BlockView& block)
    {
        EXPECT_EQ (block.firstSample, 42);
        EXPECT_EQ (block.configurationGeneration, 7u);
        EXPECT_EQ (block.numChannels, 2u);
        EXPECT_EQ (block.numSamples, 3u);
        EXPECT_EQ (std::vector<float> (block.getChannelData (0), block.getChannelData (0) + 3),
                   (std::vector<float> { 1.0f, 2.0f, 3.0f }));
        EXPECT_EQ (std::vector<float> (block.getChannelData (1), block.getChannelData (1) + 3),
                   (std::vector<float> { 11.0f, 12.0f, 13.0f }));
    }));

    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SampleBlockFifoTests, RetainsOrderWhenSlotIndexesWrap)
{
    SampleBlockFifo fifo (1, 1, 2);
    const std::array<float, 3> values { 10.0f, 11.0f, 12.0f };

    const float* first[] { &values[0] };
    const float* second[] { &values[1] };
    const float* third[] { &values[2] };
    ASSERT_TRUE (fifo.tryPush (first, 1, 1, 100));
    ASSERT_TRUE (fifo.tryPush (second, 1, 1, 101));

    std::vector<float> consumed;
    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block) { consumed.push_back (block.getChannelData (0)[0]); }));
    ASSERT_TRUE (fifo.tryPush (third, 1, 1, 102));
    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block) { consumed.push_back (block.getChannelData (0)[0]); }));
    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block) { consumed.push_back (block.getChannelData (0)[0]); }));

    EXPECT_EQ (consumed, (std::vector<float> { 10.0f, 11.0f, 12.0f }));
}

TEST (SampleBlockFifoTests, DropsWholeBlockWhenFull)
{
    SampleBlockFifo fifo (1, 4, 1);
    const std::array<float, 2> accepted { 1.0f, 2.0f };
    const std::array<float, 3> dropped { 3.0f, 4.0f, 5.0f };
    const float* acceptedChannel[] { accepted.data() };
    const float* droppedChannel[] { dropped.data() };

    ASSERT_TRUE (fifo.tryPush (acceptedChannel, 1, accepted.size(), 10));
    EXPECT_FALSE (fifo.tryPush (droppedChannel, 1, dropped.size(), 12));
    EXPECT_EQ (fifo.getDroppedBlockCount(), 1u);
    EXPECT_EQ (fifo.getDroppedSampleCount(), 3u);

    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block)
    {
        EXPECT_EQ (block.firstSample, 10);
        EXPECT_EQ (block.numSamples, 2u);
        EXPECT_EQ (block.getChannelData (0)[0], 1.0f);
        EXPECT_EQ (block.getChannelData (0)[1], 2.0f);
    }));
    EXPECT_FALSE (fifo.tryPop ([] (const auto&) {}));
}

TEST (SampleBlockFifoTests, RejectsInvalidBlocksWithoutPublishingThem)
{
    SampleBlockFifo fifo (2, 2, 2);
    const std::array<float, 3> oversized { 1.0f, 2.0f, 3.0f };
    const float* oneChannel[] { oversized.data() };
    const float* nullChannel[] { oversized.data(), nullptr };

    EXPECT_FALSE (fifo.tryPush (nullptr, 2, 1, 0));
    EXPECT_FALSE (fifo.tryPush (oneChannel, 1, 1, 0));
    EXPECT_FALSE (fifo.tryPush (nullChannel, 2, 1, 0));
    EXPECT_FALSE (fifo.tryPush (nullChannel, 2, 3, 0));
    EXPECT_EQ (fifo.getRejectedBlockCount(), 4u);
    EXPECT_EQ (fifo.getDroppedBlockCount(), 0u);
    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SampleBlockFifoTests, DroppedRangeBecomesAssemblerDiscontinuity)
{
    SampleBlockFifo fifo (1, 2, 1);
    SampleWindowAssembler assembler (1, 4, 2, 2);
    const std::array<float, 2> firstSamples { 0.0f, 1.0f };
    const std::array<float, 2> droppedSamples { 2.0f, 3.0f };
    const std::array<float, 2> laterSamples { 4.0f, 5.0f };
    const float* first[] { firstSamples.data() };
    const float* dropped[] { droppedSamples.data() };
    const float* later[] { laterSamples.data() };

    ASSERT_TRUE (fifo.tryPush (first, 1, 2, 0));
    ASSERT_FALSE (fifo.tryPush (dropped, 1, 2, 2));
    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block)
    {
        const float* channels[] { block.getChannelData (0) };
        const auto result = assembler.appendBlock (channels, 1, block.numSamples, block.firstSample);
        assembler.consumeReadyWindows ([] (const auto&) {});
        EXPECT_FALSE (result.discontinuity);
    }));

    ASSERT_TRUE (fifo.tryPush (later, 1, 2, 4));
    ASSERT_TRUE (fifo.tryPop ([&] (const auto& block)
    {
        const float* channels[] { block.getChannelData (0) };
        const auto result = assembler.appendBlock (channels, 1, block.numSamples, block.firstSample);
        assembler.consumeReadyWindows ([] (const auto&) {});
        EXPECT_TRUE (result.discontinuity);
    }));
    EXPECT_EQ (assembler.getDiscontinuityCount(), 1u);
}

TEST (SampleBlockFifoTests, QuiescentResetClearsQueueAndCounters)
{
    SampleBlockFifo fifo (1, 1, 1);
    const float sample = 1.0f;
    const float* channel[] { &sample };
    ASSERT_TRUE (fifo.tryPush (channel, 1, 1, 0));
    ASSERT_FALSE (fifo.tryPush (channel, 1, 1, 1));

    fifo.reset();

    EXPECT_EQ (fifo.getNumReady(), 0u);
    EXPECT_EQ (fifo.getDroppedBlockCount(), 0u);
    EXPECT_EQ (fifo.getDroppedSampleCount(), 0u);
    EXPECT_EQ (fifo.getRejectedBlockCount(), 0u);
    EXPECT_TRUE (fifo.tryPush (channel, 1, 1, 2));
}

TEST (SampleBlockFifoTests, ConfigurationChangeDoesNotSpliceAWindow)
{
    SampleBlockFifo fifo (1, 2, 3);
    SampleWindowAssembler assembler (1, 4, 2, 2);
    const std::array<float, 2> first { 0.0f, 1.0f };
    const std::array<float, 2> reconfigured { 2.0f, 3.0f };
    const std::array<float, 2> continued { 4.0f, 5.0f };
    const float* firstChannel[] { first.data() };
    const float* reconfiguredChannel[] { reconfigured.data() };
    const float* continuedChannel[] { continued.data() };
    ASSERT_TRUE (fifo.tryPush (firstChannel, 1, 2, 0, 1));
    ASSERT_TRUE (fifo.tryPush (reconfiguredChannel, 1, 2, 2, 2));
    ASSERT_TRUE (fifo.tryPush (continuedChannel, 1, 2, 4, 2));

    std::uint64_t activeGeneration = 0;
    std::vector<std::vector<float>> windows;
    while (fifo.tryPop ([&] (const auto& block)
    {
        if (block.configurationGeneration != activeGeneration)
        {
            assembler.reset();
            activeGeneration = block.configurationGeneration;
        }

        const float* channels[] { block.getChannelData (0) };
        const auto result = assembler.appendBlock (channels, 1, block.numSamples, block.firstSample);
        EXPECT_TRUE (result.accepted);
        assembler.consumeReadyWindows ([&] (const auto& window)
        {
            const auto channel = window.getChannel (0);
            auto& captured = windows.emplace_back();
            captured.insert (captured.end(), channel.firstData, channel.firstData + channel.firstSize);
            captured.insert (captured.end(), channel.secondData, channel.secondData + channel.secondSize);
        });
    }))
    {
    }

    ASSERT_EQ (windows.size(), 1u);
    EXPECT_EQ (windows[0], (std::vector<float> { 2.0f, 3.0f, 4.0f, 5.0f }));
}

TEST (SampleBlockFifoTests, PublishesCompleteBlocksBetweenProducerAndConsumer)
{
    constexpr std::size_t blockCount = 10000;
    constexpr std::size_t samplesPerBlock = 4;
    SampleBlockFifo fifo (2, samplesPerBlock, 3);
    std::atomic<bool> valid { true };

    std::thread consumer ([&]
    {
        std::size_t expectedBlock = 0;
        while (expectedBlock < blockCount)
        {
            if (! fifo.tryPop ([&] (const auto& block)
                {
                    if (block.firstSample != static_cast<std::int64_t> (expectedBlock * samplesPerBlock)
                        || block.numChannels != 2 || block.numSamples != samplesPerBlock)
                        valid.store (false, std::memory_order_relaxed);

                    for (std::size_t sample = 0; sample < samplesPerBlock; ++sample)
                    {
                        if (block.getChannelData (0)[sample] != static_cast<float> (expectedBlock * 10 + sample)
                            || block.getChannelData (1)[sample] != static_cast<float> (100000 + expectedBlock * 10 + sample))
                            valid.store (false, std::memory_order_relaxed);
                    }

                    ++expectedBlock;
                }))
                std::this_thread::yield();
        }
    });

    for (std::size_t block = 0; block < blockCount; ++block)
    {
        std::array<float, samplesPerBlock> channel0 {};
        std::array<float, samplesPerBlock> channel1 {};
        for (std::size_t sample = 0; sample < samplesPerBlock; ++sample)
        {
            channel0[sample] = static_cast<float> (block * 10 + sample);
            channel1[sample] = static_cast<float> (100000 + block * 10 + sample);
        }

        const float* channels[] { channel0.data(), channel1.data() };
        while (! fifo.tryPush (channels,
                               2,
                               samplesPerBlock,
                               static_cast<std::int64_t> (block * samplesPerBlock)))
            std::this_thread::yield();
    }

    consumer.join();
    EXPECT_TRUE (valid.load (std::memory_order_relaxed));
    EXPECT_EQ (fifo.getNumReady(), 0u);
}
} // namespace
