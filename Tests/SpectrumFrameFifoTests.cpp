/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "gtest/gtest.h"

#include "SpectrumFrameFifo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <vector>

namespace
{
using spectrumviewer::SpectrumFrameDescriptor;
using spectrumviewer::SpectrumFrameFifo;

SpectrumFrameDescriptor makeDescriptor (std::size_t binCount,
                                        std::uint64_t generation = 1)
{
    SpectrumFrameDescriptor result;
    result.windowSampleCount = binCount > 1 ? 2 * (binCount - 1) : 1;
    result.hopSampleCount = 1;
    result.sampleRateHz = 30000.0;
    result.binWidthHz = result.sampleRateHz / static_cast<double> (result.windowSampleCount);
    result.timeHalfBandwidth = std::min (
        2.0, 0.25 * static_cast<double> (result.windowSampleCount));
    result.taperCount = 3;
    result.configurationGeneration = generation;
    return result;
}

TEST (SpectrumFrameFifoTests, RejectsInvalidConfiguration)
{
    EXPECT_THROW ((SpectrumFrameFifo { 0, 4, 2, makeDescriptor (4) }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 1, 0, 2, makeDescriptor (4) }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 1, 4, 0, makeDescriptor (4) }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 1, 4, 2, SpectrumFrameDescriptor {} }), std::invalid_argument);
    EXPECT_THROW ((SpectrumFrameFifo { 2, 4, 2, makeDescriptor (4), { 3 } }), std::invalid_argument);
}

TEST (SpectrumFrameFifoTests, PublishesACompletePlanarFrame)
{
    SpectrumFrameFifo fifo (2, 3, 2, makeDescriptor (3, 7), { 4, 9 });
    const std::array<float, 6> powers { 1.0f, 2.0f, 3.0f, 11.0f, 12.0f, 13.0f };
    ASSERT_TRUE (fifo.tryPush (powers.data(), 2, 3, 42, 9));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
                                    {
        EXPECT_EQ (frame.firstSample, 42);
        EXPECT_EQ (frame.configurationGeneration, 7u);
        EXPECT_EQ (frame.sequence, 9u);
        EXPECT_EQ (frame.numChannels, 2u);
        EXPECT_EQ (frame.numBins, 3u);
        EXPECT_EQ (frame.getSourceChannelIndex (0), 4);
        EXPECT_EQ (frame.getSourceChannelIndex (1), 9);
        EXPECT_EQ (frame.getSourceChannelIndex (2), -1);
        EXPECT_EQ (frame.getChannelData (2), nullptr);
        EXPECT_DOUBLE_EQ (frame.descriptor.sampleRateHz, 30000.0);
        EXPECT_EQ (frame.descriptor.windowSampleCount, 4u);
        EXPECT_EQ (std::vector<float> (frame.getChannelData (0), frame.getChannelData (0) + 3),
                   (std::vector<float> { 1.0f, 2.0f, 3.0f }));
        EXPECT_EQ (std::vector<float> (frame.getChannelData (1), frame.getChannelData (1) + 3),
                   (std::vector<float> { 11.0f, 12.0f, 13.0f })); }));
}

TEST (SpectrumFrameFifoTests, PublishesReducedMeansPeaksFrequenciesAndUnits)
{
    SpectrumFrameFifo fifo (2, 5, 2, makeDescriptor (5, 3), { 4, 9 }, { "uV", "mV" });
    const std::array<float, 6> means { 1, 2, 3, 11, 12, 13 };
    const std::array<float, 6> peaks { 4, 5, 6, 14, 15, 16 };
    const std::array<float, 3> frequencies { 10, 20, 30 };
    ASSERT_TRUE (fifo.tryPushReduced (means.data(), peaks.data(), frequencies.data(), 2, 3, 42, 7, spectrumviewer::FrequencyScale::logarithmic, 10.0, 40.0));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
                                    {
        EXPECT_TRUE (frame.reducedForDisplay);
        EXPECT_EQ (frame.numBins, 3u);
        EXPECT_EQ (frame.frequencyScale, spectrumviewer::FrequencyScale::logarithmic);
        EXPECT_DOUBLE_EQ (frame.minimumFrequencyHz, 10.0);
        EXPECT_DOUBLE_EQ (frame.maximumFrequencyHz, 40.0);
        EXPECT_STREQ (frame.getSourceChannelUnit (0), "uV");
        EXPECT_STREQ (frame.getSourceChannelUnit (1), "mV");
        EXPECT_EQ (std::vector<float> (frame.frequenciesHz, frame.frequenciesHz + 3),
                   (std::vector<float> { 10, 20, 30 }));
        EXPECT_EQ (std::vector<float> (frame.getChannelData (1),
                                       frame.getChannelData (1) + 3),
                   (std::vector<float> { 11, 12, 13 }));
        EXPECT_EQ (std::vector<float> (frame.getChannelPeakData (1),
                                       frame.getChannelPeakData (1) + 3),
                   (std::vector<float> { 14, 15, 16 })); }));
}

TEST (SpectrumFrameFifoTests, DrainsStaleFramesAndReturnsOnlyNewest)
{
    SpectrumFrameFifo fifo (1, 1, 3, makeDescriptor (1));
    const std::array<float, 3> values { 1.0f, 2.0f, 3.0f };
    ASSERT_TRUE (fifo.tryPush (&values[0], 1, 1, 10, 0));
    ASSERT_TRUE (fifo.tryPush (&values[1], 1, 1, 11, 1));
    ASSERT_TRUE (fifo.tryPush (&values[2], 1, 1, 12, 2));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
                                    {
        EXPECT_EQ (frame.sequence, 2u);
        EXPECT_EQ (frame.getChannelData (0)[0], 3.0f); }));
    EXPECT_EQ (fifo.getStaleFrameCount(), 2u);
    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SpectrumFrameFifoTests, FindsNewestFrameAcrossSlotWrap)
{
    SpectrumFrameFifo fifo (1, 1, 2, makeDescriptor (1));
    const std::array<float, 4> values { 1.0f, 2.0f, 3.0f, 4.0f };

    ASSERT_TRUE (fifo.tryPush (&values[0], 1, 1, 0, 0));
    ASSERT_TRUE (fifo.tryPush (&values[1], 1, 1, 1, 1));
    ASSERT_TRUE (fifo.tryPopLatest ([] (const auto&) {}));
    ASSERT_TRUE (fifo.tryPush (&values[2], 1, 1, 2, 2));
    ASSERT_TRUE (fifo.tryPush (&values[3], 1, 1, 3, 3));

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
                                    {
        EXPECT_EQ (frame.sequence, 3u);
        EXPECT_EQ (frame.getChannelData (0)[0], 4.0f); }));
}

TEST (SpectrumFrameFifoTests, DropsCompleteFramesWhenFull)
{
    SpectrumFrameFifo fifo (2, 2, 1, makeDescriptor (2));
    const std::array<float, 4> accepted { 1.0f, 2.0f, 3.0f, 4.0f };
    const std::array<float, 4> dropped { 5.0f, 6.0f, 7.0f, 8.0f };

    ASSERT_TRUE (fifo.tryPush (accepted.data(), 2, 2, 0, 0));
    EXPECT_FALSE (fifo.tryPush (dropped.data(), 2, 2, 1, 1));
    EXPECT_EQ (fifo.getDroppedFrameCount(), 1u);

    ASSERT_TRUE (fifo.tryPopLatest ([&] (const auto& frame)
                                    {
        EXPECT_EQ (frame.sequence, 0u);
        EXPECT_EQ (frame.getChannelData (1)[1], 4.0f); }));
}

TEST (SpectrumFrameFifoTests, RejectsMismatchedFramesWithoutPublishing)
{
    SpectrumFrameFifo fifo (2, 2, 1, makeDescriptor (2));
    const std::array<float, 4> powers { 1.0f, 2.0f, 3.0f, 4.0f };
    EXPECT_FALSE (fifo.tryPush (nullptr, 2, 2, 0, 0));
    EXPECT_FALSE (fifo.tryPush (powers.data(), 1, 2, 0, 0));
    EXPECT_FALSE (fifo.tryPush (powers.data(), 2, 1, 0, 0));
    EXPECT_EQ (fifo.getRejectedFrameCount(), 3u);
    EXPECT_EQ (fifo.getNumReady(), 0u);
}

TEST (SpectrumFrameFifoTests, PublishesOnlyCompleteFramesBetweenWorkerAndUi)
{
    constexpr std::uint64_t frameCount = 10000;
    constexpr std::size_t channelCount = 2;
    constexpr std::size_t binCount = 4;
    SpectrumFrameFifo fifo (channelCount, binCount, 3, makeDescriptor (binCount));
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
        } });

    for (std::uint64_t sequence = 0; sequence < frameCount; ++sequence)
    {
        std::array<float, channelCount * binCount> frame {};
        for (std::size_t channel = 0; channel < channelCount; ++channel)
            for (std::size_t bin = 0; bin < binCount; ++bin)
                frame[channel * binCount + bin] = static_cast<float> (
                    sequence * 100 + channel * 10 + bin);

        while (! fifo.tryPush (frame.data(), channelCount, binCount, 0, sequence))
            std::this_thread::yield();
    }

    consumer.join();
    EXPECT_TRUE (valid.load (std::memory_order_relaxed));
    EXPECT_EQ (fifo.getNumReady(), 0u);
}
} // namespace
