/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI

    ------------------------------------------------------------------
*/

#ifndef SPSC_SAMPLE_QUEUE_H_INCLUDED
#define SPSC_SAMPLE_QUEUE_H_INCLUDED

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace spectrumviewer
{
/**
    A bounded single-producer/single-consumer queue of planar sample blocks.

    Construction and destruction happen off the real-time path. Exactly one thread may
    call tryPush(), and exactly one other thread may call tryConsume(). A full queue
    rejects the complete incoming block instead of overwriting a slot owned by the
    consumer. BlockView data remains valid only for the duration of the consumer call.
*/
class SpscSampleQueue
{
    static_assert (std::atomic<std::size_t>::is_always_lock_free,
                   "SpscSampleQueue requires lock-free index atomics");
    static_assert (std::atomic<std::uint64_t>::is_always_lock_free,
                   "SpscSampleQueue requires lock-free diagnostic counters");

public:
    enum class PushResult
    {
        pushed,
        full,
        invalidShape
    };

    class BlockView
    {
    public:
        const float* getChannelData (std::size_t channel) const noexcept
        {
            return data + channel * channelStride;
        }

        std::uint64_t firstSample = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;

    private:
        friend class SpscSampleQueue;

        const float* data = nullptr;
        std::size_t channelStride = 0;
    };

    SpscSampleQueue (std::size_t numChannels,
                     std::size_t maxSamplesPerBlock,
                     std::size_t capacityInBlocks)
        : channelCount (numChannels),
          blockSampleCapacity (maxSamplesPerBlock),
          slots (capacityInBlocks + 1)
    {
        if (numChannels == 0 || maxSamplesPerBlock == 0 || capacityInBlocks == 0)
            throw std::invalid_argument ("SpscSampleQueue dimensions must be non-zero");

        for (auto& slot : slots)
            slot.samples.resize (channelCount * blockSampleCapacity);
    }

    PushResult tryPush (const float* const* source,
                        std::size_t numChannels,
                        std::size_t numSamples,
                        std::uint64_t firstSample) noexcept
    {
        if (source == nullptr || numChannels != channelCount || numSamples == 0 || numSamples > blockSampleCapacity)
            return PushResult::invalidShape;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            if (source[channel] == nullptr)
                return PushResult::invalidShape;
        }

        const auto currentWrite = writeIndex.load (std::memory_order_relaxed);
        const auto nextWrite = increment (currentWrite);

        if (nextWrite == readIndex.load (std::memory_order_acquire))
        {
            rejectedBlockCount.fetch_add (1, std::memory_order_relaxed);
            rejectedSampleCount.fetch_add (numSamples, std::memory_order_relaxed);
            return PushResult::full;
        }

        auto& slot = slots[currentWrite];
        slot.firstSample = firstSample;
        slot.numSamples = numSamples;

        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            std::memcpy (slot.samples.data() + channel * blockSampleCapacity,
                         source[channel],
                         numSamples * sizeof (float));
        }

        writeIndex.store (nextWrite, std::memory_order_release);
        return PushResult::pushed;
    }

    template <typename Consumer>
    bool tryConsume (Consumer&& consumer)
    {
        const auto currentRead = readIndex.load (std::memory_order_relaxed);

        if (currentRead == writeIndex.load (std::memory_order_acquire))
            return false;

        const auto& slot = slots[currentRead];
        BlockView view;
        view.data = slot.samples.data();
        view.channelStride = blockSampleCapacity;
        view.firstSample = slot.firstSample;
        view.numChannels = channelCount;
        view.numSamples = slot.numSamples;

        std::forward<Consumer> (consumer) (view);
        readIndex.store (increment (currentRead), std::memory_order_release);
        return true;
    }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getMaxSamplesPerBlock() const noexcept { return blockSampleCapacity; }
    std::size_t getCapacityInBlocks() const noexcept { return slots.size() - 1; }

    std::uint64_t getRejectedBlockCount() const noexcept
    {
        return rejectedBlockCount.load (std::memory_order_relaxed);
    }

    std::uint64_t getRejectedSampleCount() const noexcept
    {
        return rejectedSampleCount.load (std::memory_order_relaxed);
    }

private:
    struct Slot
    {
        std::vector<float> samples;
        std::uint64_t firstSample = 0;
        std::size_t numSamples = 0;
    };

    std::size_t increment (std::size_t index) const noexcept
    {
        ++index;
        return index == slots.size() ? 0 : index;
    }

    const std::size_t channelCount;
    const std::size_t blockSampleCapacity;
    std::vector<Slot> slots;

    alignas (64) std::atomic<std::size_t> writeIndex { 0 };
    alignas (64) std::atomic<std::size_t> readIndex { 0 };
    std::atomic<std::uint64_t> rejectedBlockCount { 0 };
    std::atomic<std::uint64_t> rejectedSampleCount { 0 };
};
} // namespace spectrumviewer

#endif // SPSC_SAMPLE_QUEUE_H_INCLUDED
