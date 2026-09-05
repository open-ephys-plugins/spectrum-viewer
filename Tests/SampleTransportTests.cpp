#include <gtest/gtest.h>

#include "SampleWindowAssembler.h"
#include "SpscSampleQueue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace
{
using spectrumviewer::SampleWindowAssembler;
using spectrumviewer::SpscSampleQueue;

struct CapturedWindow
{
    std::uint64_t firstSample;
    std::vector<std::vector<float>> channels;
};

template <typename View>
std::vector<std::vector<float>> copyChannels (const View& view)
{
    std::vector<std::vector<float>> result;

    for (std::size_t channel = 0; channel < view.numChannels; ++channel)
    {
        const auto* begin = view.getChannelData (channel);
        result.emplace_back (begin, begin + view.numSamples);
    }

    return result;
}

TEST (SpscSampleQueueTests, PreservesBlockMetadataAndChannelData)
{
    SpscSampleQueue queue (2, 4, 2);
    const std::array<float, 3> channel0 { 1.0f, 2.0f, 3.0f };
    const std::array<float, 3> channel1 { 11.0f, 12.0f, 13.0f };
    const float* source[] { channel0.data(), channel1.data() };

    EXPECT_EQ (queue.tryPush (source, 2, 3, 100), SpscSampleQueue::PushResult::pushed);

    bool consumed = queue.tryConsume ([&] (const SpscSampleQueue::BlockView& block)
    {
        EXPECT_EQ (block.firstSample, 100u);
        EXPECT_EQ (block.numChannels, 2u);
        EXPECT_EQ (block.numSamples, 3u);
        EXPECT_EQ (copyChannels (block), (std::vector<std::vector<float>> { { 1.0f, 2.0f, 3.0f }, { 11.0f, 12.0f, 13.0f } }));
    });

    EXPECT_TRUE (consumed);
    EXPECT_FALSE (queue.tryConsume ([] (const auto&) {}));
}

TEST (SpscSampleQueueTests, RejectsZeroSizedConfiguration)
{
    EXPECT_THROW ((SpscSampleQueue { 0, 4, 2 }), std::invalid_argument);
    EXPECT_THROW ((SpscSampleQueue { 2, 0, 2 }), std::invalid_argument);
    EXPECT_THROW ((SpscSampleQueue { 2, 4, 0 }), std::invalid_argument);
}

TEST (SpscSampleQueueTests, RejectsFullQueueWithoutOverwritingUnreadData)
{
    SpscSampleQueue queue (1, 1, 2);
    const std::array<float, 3> values { 1.0f, 2.0f, 3.0f };

    for (std::size_t index = 0; index < 2; ++index)
    {
        const float* source[] { &values[index] };
        ASSERT_EQ (queue.tryPush (source, 1, 1, index), SpscSampleQueue::PushResult::pushed);
    }

    const float* rejected[] { &values[2] };
    EXPECT_EQ (queue.tryPush (rejected, 1, 1, 2), SpscSampleQueue::PushResult::full);
    EXPECT_EQ (queue.getRejectedBlockCount(), 1u);
    EXPECT_EQ (queue.getRejectedSampleCount(), 1u);

    std::vector<float> consumed;
    while (queue.tryConsume ([&] (const auto& block) { consumed.push_back (block.getChannelData (0)[0]); }))
    {
    }

    EXPECT_EQ (consumed, (std::vector<float> { 1.0f, 2.0f }));
}

TEST (SpscSampleQueueTests, ReusesSlotsAfterWraparound)
{
    SpscSampleQueue queue (1, 1, 2);
    std::vector<float> consumed;

    for (std::uint64_t sample = 0; sample < 20; ++sample)
    {
        const auto value = static_cast<float> (sample);
        const float* source[] { &value };
        ASSERT_EQ (queue.tryPush (source, 1, 1, sample), SpscSampleQueue::PushResult::pushed);
        ASSERT_TRUE (queue.tryConsume ([&] (const auto& block)
        {
            EXPECT_EQ (block.firstSample, sample);
            consumed.push_back (block.getChannelData (0)[0]);
        }));
    }

    ASSERT_EQ (consumed.size(), 20u);
    for (std::size_t index = 0; index < consumed.size(); ++index)
        EXPECT_EQ (consumed[index], static_cast<float> (index));
}

TEST (SpscSampleQueueTests, RejectsInvalidShapes)
{
    SpscSampleQueue queue (2, 4, 2);
    const std::array<float, 5> values {};
    const float* oneChannel[] { values.data() };
    const float* nullChannel[] { values.data(), nullptr };

    EXPECT_EQ (queue.tryPush (oneChannel, 1, 1, 0), SpscSampleQueue::PushResult::invalidShape);
    EXPECT_EQ (queue.tryPush (nullChannel, 2, 1, 0), SpscSampleQueue::PushResult::invalidShape);
    EXPECT_EQ (queue.tryPush (nullChannel, 2, 5, 0), SpscSampleQueue::PushResult::invalidShape);
    EXPECT_EQ (queue.getRejectedBlockCount(), 0u);
    EXPECT_EQ (queue.getRejectedSampleCount(), 0u);
}

TEST (SpscSampleQueueTests, TransfersOrderedBlocksBetweenThreads)
{
    constexpr std::uint64_t blockCount = 20000;
    SpscSampleQueue queue (2, 1, 64);
    std::atomic<bool> producerFinished { false };
    std::vector<std::array<float, 2>> consumed;
    consumed.reserve (blockCount);

    std::thread producer ([&]
    {
        for (std::uint64_t sample = 0; sample < blockCount; ++sample)
        {
            const std::array<float, 2> values { static_cast<float> (sample), static_cast<float> (sample + 100000) };
            const float* source[] { &values[0], &values[1] };

            while (queue.tryPush (source, 2, 1, sample) == SpscSampleQueue::PushResult::full)
                std::this_thread::yield();
        }

        producerFinished.store (true, std::memory_order_release);
    });

    while (! producerFinished.load (std::memory_order_acquire) || consumed.size() < blockCount)
    {
        if (! queue.tryConsume ([&] (const auto& block)
            {
                consumed.push_back ({ block.getChannelData (0)[0], block.getChannelData (1)[0] });
            }))
        {
            std::this_thread::yield();
        }
    }

    producer.join();
    ASSERT_EQ (consumed.size(), blockCount);

    for (std::size_t sample = 0; sample < consumed.size(); ++sample)
    {
        EXPECT_EQ (consumed[sample][0], static_cast<float> (sample));
        EXPECT_EQ (consumed[sample][1], static_cast<float> (sample + 100000));
    }
}

TEST (SampleWindowAssemblerTests, ProducesExactOverlappingWindowsAcrossCallbackPartitions)
{
    SampleWindowAssembler assembler (1, 4, 2);
    const std::array<float, 10> samples { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f };
    const std::array<std::size_t, 4> partitions { 1, 3, 2, 4 };
    std::vector<CapturedWindow> windows;
    std::size_t offset = 0;

    for (const auto partition : partitions)
    {
        const float* source[] { samples.data() + offset };
        const auto result = assembler.append (source, 1, partition, offset, [&] (const auto& window)
        {
            windows.push_back ({ window.firstSample, copyChannels (window) });
        });
        EXPECT_TRUE (result.accepted);
        EXPECT_FALSE (result.discontinuity);
        offset += partition;
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

TEST (SampleWindowAssemblerTests, RejectsZeroSizedConfiguration)
{
    EXPECT_THROW ((SampleWindowAssembler { 0, 4, 2 }), std::invalid_argument);
    EXPECT_THROW ((SampleWindowAssembler { 1, 0, 2 }), std::invalid_argument);
    EXPECT_THROW ((SampleWindowAssembler { 1, 4, 0 }), std::invalid_argument);
}

TEST (SampleWindowAssemblerTests, CallbackPartitioningDoesNotChangeOutput)
{
    std::vector<float> samples (97);
    for (std::size_t index = 0; index < samples.size(); ++index)
        samples[index] = static_cast<float> (index);

    auto assemble = [&] (const std::vector<std::size_t>& partitions)
    {
        SampleWindowAssembler assembler (1, 11, 4);
        std::vector<CapturedWindow> windows;
        std::size_t offset = 0;

        for (const auto partition : partitions)
        {
            const float* source[] { samples.data() + offset };
            const auto result = assembler.append (source, 1, partition, offset, [&] (const auto& window)
            {
                windows.push_back ({ window.firstSample, copyChannels (window) });
            });
            EXPECT_TRUE (result.accepted);
            offset += partition;
        }

        EXPECT_EQ (offset, samples.size());
        return windows;
    };

    const auto oneBlock = assemble ({ samples.size() });
    const auto irregularBlocks = assemble ({ 3, 1, 17, 4, 29, 2, 7, 13, 21 });

    ASSERT_EQ (irregularBlocks.size(), oneBlock.size());
    for (std::size_t index = 0; index < oneBlock.size(); ++index)
    {
        EXPECT_EQ (irregularBlocks[index].firstSample, oneBlock[index].firstSample);
        EXPECT_EQ (irregularBlocks[index].channels, oneBlock[index].channels);
    }
}

TEST (SampleWindowAssemblerTests, RandomCallbackPartitionsMatchSingleBlockReference)
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

    const std::array<std::pair<std::size_t, std::size_t>, 5> layouts {
        std::pair<std::size_t, std::size_t> { 1, 1 },
        std::pair<std::size_t, std::size_t> { 8, 3 },
        std::pair<std::size_t, std::size_t> { 31, 7 },
        std::pair<std::size_t, std::size_t> { 64, 32 },
        std::pair<std::size_t, std::size_t> { 127, 127 }
    };

    for (const auto [windowSize, hopSize] : layouts)
    {
        auto assemble = [&] (const std::vector<std::size_t>& partitions)
        {
            SampleWindowAssembler assembler (numChannels, windowSize, hopSize);
            std::vector<CapturedWindow> windows;
            std::size_t offset = 0;

            for (const auto partition : partitions)
            {
                std::array<const float*, numChannels> source;
                for (std::size_t channel = 0; channel < numChannels; ++channel)
                    source[channel] = samples[channel].data() + offset;

                const auto result = assembler.append (source.data(), numChannels, partition, 5000 + offset, [&] (const auto& window)
                {
                    windows.push_back ({ window.firstSample, copyChannels (window) });
                });
                EXPECT_TRUE (result.accepted);
                EXPECT_FALSE (result.discontinuity);
                offset += partition;
            }

            EXPECT_EQ (offset, totalSamples);
            return windows;
        };

        const auto reference = assemble ({ totalSamples });
        std::mt19937 generator (12345);
        std::uniform_int_distribution<std::size_t> blockSize (1, 53);
        std::vector<std::size_t> randomPartitions;
        std::size_t remaining = totalSamples;

        while (remaining > 0)
        {
            const auto partition = std::min (remaining, blockSize (generator));
            randomPartitions.push_back (partition);
            remaining -= partition;
        }

        const auto partitioned = assemble (randomPartitions);
        ASSERT_EQ (partitioned.size(), reference.size());

        for (std::size_t index = 0; index < reference.size(); ++index)
        {
            EXPECT_EQ (partitioned[index].firstSample, reference[index].firstSample);
            EXPECT_EQ (partitioned[index].channels, reference[index].channels);
        }
    }
}

TEST (SampleWindowAssemblerTests, DiscontinuityResetsPartialWindow)
{
    SampleWindowAssembler assembler (1, 4, 2);
    const std::array<float, 3> partial { 0.0f, 1.0f, 2.0f };
    const std::array<float, 6> afterGap { 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f };
    std::vector<CapturedWindow> windows;

    const float* partialSource[] { partial.data() };
    const auto first = assembler.append (partialSource, 1, partial.size(), 0, [&] (const auto& window)
    {
        windows.push_back ({ window.firstSample, copyChannels (window) });
    });
    EXPECT_FALSE (first.discontinuity);
    EXPECT_EQ (first.windowsEmitted, 0u);

    const float* gapSource[] { afterGap.data() };
    const auto second = assembler.append (gapSource, 1, afterGap.size(), 10, [&] (const auto& window)
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

TEST (SampleWindowAssemblerTests, PreservesMultipleChannels)
{
    SampleWindowAssembler assembler (2, 3, 3);
    const std::array<float, 3> channel0 { 1.0f, 2.0f, 3.0f };
    const std::array<float, 3> channel1 { 11.0f, 12.0f, 13.0f };
    const float* source[] { channel0.data(), channel1.data() };
    std::vector<std::vector<float>> captured;

    const auto result = assembler.append (source, 2, 3, 50, [&] (const auto& window)
    {
        captured = copyChannels (window);
    });

    EXPECT_TRUE (result.accepted);
    EXPECT_EQ (result.windowsEmitted, 1u);
    ASSERT_EQ (captured.size(), 2u);
    EXPECT_EQ (captured[0], (std::vector<float> { 1.0f, 2.0f, 3.0f }));
    EXPECT_EQ (captured[1], (std::vector<float> { 11.0f, 12.0f, 13.0f }));
}

TEST (SampleTransportTests, QueueFeedsWindowAssemblerWithoutLosingCallbackTails)
{
    SpscSampleQueue queue (1, 5, 4);
    SampleWindowAssembler assembler (1, 5, 3);
    const std::array<float, 11> samples { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f };
    const std::array<std::size_t, 3> partitions { 2, 5, 4 };
    std::vector<CapturedWindow> windows;
    std::size_t offset = 0;

    for (const auto partition : partitions)
    {
        const float* source[] { samples.data() + offset };
        ASSERT_EQ (queue.tryPush (source, 1, partition, offset), SpscSampleQueue::PushResult::pushed);
        offset += partition;

        ASSERT_TRUE (queue.tryConsume ([&] (const auto& block)
        {
            const float* queuedChannels[] { block.getChannelData (0) };
            assembler.append (queuedChannels, block.numChannels, block.numSamples, block.firstSample, [&] (const auto& window)
            {
                windows.push_back ({ window.firstSample, copyChannels (window) });
            });
        }));
    }

    ASSERT_EQ (windows.size(), 3u);
    EXPECT_EQ (windows[0].channels[0], (std::vector<float> { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f }));
    EXPECT_EQ (windows[1].channels[0], (std::vector<float> { 3.0f, 4.0f, 5.0f, 6.0f, 7.0f }));
    EXPECT_EQ (windows[2].channels[0], (std::vector<float> { 6.0f, 7.0f, 8.0f, 9.0f, 10.0f }));
}
} // namespace
