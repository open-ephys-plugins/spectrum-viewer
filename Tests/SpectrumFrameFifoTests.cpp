/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "gtest/gtest.h"

#include "SpectrumFrameFifo.h"

#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
using spectrumviewer::SpectrumFrameFifo;

TEST (SpectrumFrameFifoTests, RejectsInvalidConfiguration)
{
    EXPECT_THROW ((SpectrumFrameFifo { 0, 4, 2 }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 1, 0, 2 }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 1, 4, 0 }), std::invalid_argument);
}

TEST (SpectrumFrameFifoTests, PublishesACompletePlanarFrame)
{
    SpectrumFrameFifo fifo (2, 3, 2);
    const std::array<float, 6> powers { 1.0f, 2.0f, 3.0f, 11.0f, 12.0f, 13.0f };
    ASSERT_TRUE (fifo.tryPush (powers.data(), 2, 3, 42, 7, 9));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
    {
        EXPECT_EQ (frame.firstSample, 42);
        EXPECT_EQ (frame.configurationGeneration, 7u);
        EXPECT_EQ (frame.sequence, 9u);
        EXPECT_EQ (frame.numChannels, 2u);
        EXPECT_EQ (frame.numBins, 3u);
        EXPECT_EQ (std::vector<float> (frame.getChannelData (0), frame.getChannelData (0) + 3),
                   (std::vector<float> { 1.0f, 2.0f, 3.0f }));
        EXPECT_EQ (std::vector<float> (frame.getChannelData (1), frame.getChannelData (1) + 3),
                   (std::vector<float> { 11.0f, 12.0f, 13.0f }));
    }));
}

TEST (SpectrumFrameFifoTests, DrainsStaleFramesAndReturnsOnlyNewest)
{
    SpectrumFrameFifo fifo (1, 1, 3);
    const std::array<float, 3> values { 1.0f, 2.0f, 3.0f };
    ASSERT_TRUE (fifo.tryPush (&values[0], 1, 1, 10, 1, 0));
    ASSERT_TRUE (fifo.tryPush (&values[1], 1, 1, 11, 1, 1));
    ASSERT_TRUE (fifo.tryPush (&values[2], 1, 1, 12, 1, 2));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
    {
        EXPECT_EQ (frame.sequence, 2u);
        EXPECT_EQ (frame.getChannelData (0)[0], 3.0f);
    }));
    EXPECT_EQ (fifo.getStaleFrameCount(), 2u);
    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SpectrumFrameFifoTests, FindsNewestFrameAcrossSlotWrap)
{
    SpectrumFrameFifo fifo (1, 1, 2);
    const std::array<float, 4> values { 1.0f, 2.0f, 3.0f, 4.0f };

    ASSERT_TRUE (fifo.tryPush (&values[0], 1, 1, 0, 1, 0));
    ASSERT_TRUE (fifo.tryPush (&values[1], 1, 1, 1, 1, 1));
    ASSERT_TRUE (fifo.tryPopLatest ([] (const auto&) {}));
    ASSERT_TRUE (fifo.tryPush (&values[2], 1, 1, 2, 1, 2));
    ASSERT_TRUE (fifo.tryPush (&values[3], 1, 1, 3, 1, 3));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
    {
        EXPECT_EQ (frame.sequence, 3u);
        EXPECT_EQ (frame.getChannelData (0)[0], 4.0f);
    }));
}

TEST (SpectrumFrameFifoTests, DropsCompleteFramesWhenFull)
{
    SpectrumFrameFifo fifo (2, 2, 1);
    const std::array<float, 4> accepted { 1.0f, 2.0f, 3.0f, 4.0f };
    const std::array<float, 4> dropped { 5.0f, 6.0f, 7.0f, 8.0f };

    ASSERT_TRUE (fifo.tryPush (accepted.data(), 2, 2, 0, 1, 0));
    EXPECT_FALSE (fifo.tryPush (dropped.data(), 2, 2, 1, 1, 1));
    EXPECT_EQ (fifo.getDroppedFrameCount(), 1u);

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
    {
        EXPECT_EQ (frame.sequence, 0u);
        EXPECT_EQ (frame.getChannelData (1)[1], 4.0f);
    }));
}

TEST (SpectrumFrameFifoTests, RejectsMismatchedFramesWithoutPublishing)
{
    SpectrumFrameFifo fifo (2, 2, 1);
    const std::array<float, 4> powers { 1.0f, 2.0f, 3.0f, 4.0f };
    EXPECT_FALSE (fifo.tryPush (nullptr, 2, 2, 0, 1, 0));
    EXPECT_FALSE (fifo.tryPush (powers.data(), 1, 2, 0, 1, 0));
    EXPECT_FALSE (fifo.tryPush (powers.data(), 2, 1, 0, 1, 0));
    EXPECT_EQ (fifo.getRejectedFrameCount(), 3u);
    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SpectrumFrameFifoTests, PublishesOnlyCompleteFramesBetweenWorkerAndUi)
{
    constexpr std::uint64_t frameCount = 10000;
    constexpr std::size_t channelCount = 2;
    constexpr std::size_t binCount = 4;
    SpectrumFrameFifo fifo (channelCount, binCount, 3);
    std::atomic<bool> valid { true };

    std::thread consumer ([&]
    {
        std::uint64_t previousSequence = 0;
        bool receivedFrame = false;
        while (! receivedFrame || previousSequence + 1 < frameCount)
        {
            if (! fifo.tryPopLatest ([&] (const auto& frame)
                {
                    if (receivedFrame && frame.sequence <= previousSequence)
                        valid.store (false, std::memory_order_relaxed);

                    for (std::size_t channel = 0; channel < channelCount; ++channel)
                    {
                        for (std::size_t bin = 0; bin < binCount; ++bin)
                        {
                            const auto expected = static_cast<float> (
                                frame.sequence * 100 + channel * 10 + bin);
                            if (frame.getChannelData (channel)[bin] != expected)
                                valid.store (false, std::memory_order_relaxed);
                        }
                    }

                    previousSequence = frame.sequence;
                    receivedFrame = true;
                }))
                std::this_thread::yield();
        }
    });

    for (std::uint64_t sequence = 0; sequence < frameCount; ++sequence)
    {
        std::array<float, channelCount * binCount> frame {};
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            for (std::size_t bin = 0; bin < binCount; ++bin)
                frame[channel * binCount + bin] = static_cast<float> (
                    sequence * 100 + channel * 10 + bin);

        while (! fifo.tryPush (frame.data(), channelCount, binCount, 0, 1, sequence))
            std::this_thread::yield();
    }

    consumer.join();
    EXPECT_TRUE (valid.load (std::memory_order_relaxed));
    EXPECT_EQ (fifo.getNumReady(), 0u);
}
} // namespace
