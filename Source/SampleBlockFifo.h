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

#ifndef SAMPLE_BLOCK_FIFO_H_INCLUDED
#define SAMPLE_BLOCK_FIFO_H_INCLUDED

#include <AppConfig.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace spectrumviewer
{
/**
    Transfers bounded planar sample blocks from one producer to one consumer.

    Storage is allocated by the constructor. tryPush() and tryPop() neither allocate nor
    lock. The producer drops a whole block when the queue is full or the block exceeds
    its configured dimensions; it never overwrites unread data.

    Exactly one thread may call tryPush(), and exactly one other thread may call
    tryPop(). reset() is only safe while both threads are stopped.
*/
class SampleBlockFifo
{
public:
    class BlockView
    {
    public:
        const float* getChannelData (std::size_t channel) const noexcept
        {
            return data + channel * channelStride;
        }

        std::int64_t firstSample = 0;
        std::uint64_t configurationGeneration = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;

    private:
        friend class SampleBlockFifo;
        const float* data = nullptr;
        std::size_t channelStride = 0;
    };

    SampleBlockFifo (std::size_t numChannels,
                     std::size_t maxSamplesPerBlock,
                     std::size_t capacity)
        : channelCapacity (numChannels),
          sampleCapacity (maxSamplesPerBlock),
          slotCapacity (capacity),
          fifo (checkedFifoSize (capacity)),
          metadata (capacity + 1),
          samples (checkedSampleCount (numChannels, maxSamplesPerBlock, capacity + 1))
    {
        if (numChannels == 0 || maxSamplesPerBlock == 0)
            throw std::invalid_argument ("SampleBlockFifo dimensions must be non-zero");
    }

    bool tryPush (const float* const* source,
                  std::size_t numChannels,
                  std::size_t numSamples,
                  std::int64_t firstSample,
                  std::uint64_t configurationGeneration = 0) noexcept
    {
        if (! isValidBlock (source, numChannels, numSamples))
        {
            rejectedBlocks.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        auto writer = fifo.write (1);
        if (writer.blockSize1 + writer.blockSize2 != 1)
        {
            droppedBlocks.fetch_add (1, std::memory_order_relaxed);
            droppedSamples.fetch_add (numSamples, std::memory_order_relaxed);
            return false;
        }

        const auto slot = static_cast<std::size_t> (writer.startIndex1);
        auto* destination = samples.data() + slot * channelCapacity * sampleCapacity;

        for (std::size_t channel = 0; channel < numChannels; ++channel)
        {
            std::memcpy (destination + channel * sampleCapacity,
                         source[channel],
                         numSamples * sizeof (float));
        }

        metadata[slot] = { firstSample, configurationGeneration, numChannels, numSamples };
        return true;
    }

    template <typename Consumer>
    bool tryPop (Consumer&& consumer)
    {
        auto reader = fifo.read (1);
        if (reader.blockSize1 + reader.blockSize2 != 1)
            return false;

        const auto slot = static_cast<std::size_t> (reader.startIndex1);
        const auto& slotMetadata = metadata[slot];
        BlockView view;
        view.data = samples.data() + slot * channelCapacity * sampleCapacity;
        view.channelStride = sampleCapacity;
        view.firstSample = slotMetadata.firstSample;
        view.configurationGeneration = slotMetadata.configurationGeneration;
        view.numChannels = slotMetadata.numChannels;
        view.numSamples = slotMetadata.numSamples;
        consumer (view);
        return true;
    }

    /** Clears queued blocks and counters. Call only when producer and consumer are stopped. */
    void reset() noexcept
    {
        fifo.reset();
        droppedBlocks.store (0, std::memory_order_relaxed);
        droppedSamples.store (0, std::memory_order_relaxed);
        rejectedBlocks.store (0, std::memory_order_relaxed);
    }

    std::size_t getChannelCapacity() const noexcept { return channelCapacity; }
    std::size_t getMaxSamplesPerBlock() const noexcept { return sampleCapacity; }
    std::size_t getCapacity() const noexcept { return slotCapacity; }
    std::size_t getNumReady() const noexcept { return static_cast<std::size_t> (fifo.getNumReady()); }
    std::uint64_t getDroppedBlockCount() const noexcept { return droppedBlocks.load (std::memory_order_relaxed); }
    std::uint64_t getDroppedSampleCount() const noexcept { return droppedSamples.load (std::memory_order_relaxed); }
    std::uint64_t getRejectedBlockCount() const noexcept { return rejectedBlocks.load (std::memory_order_relaxed); }

private:
    struct Metadata
    {
        std::int64_t firstSample = 0;
        std::uint64_t configurationGeneration = 0;
        std::size_t numChannels = 0;
        std::size_t numSamples = 0;
    };

    static int checkedFifoSize (std::size_t capacity)
    {
        if (capacity == 0 || capacity >= static_cast<std::size_t> (std::numeric_limits<int>::max()))
            throw std::invalid_argument ("SampleBlockFifo capacity is out of range");
        return static_cast<int> (capacity + 1);
    }

    static std::size_t checkedSampleCount (std::size_t numChannels,
                                           std::size_t maxSamplesPerBlock,
                                           std::size_t capacity)
    {
        if (numChannels == 0 || maxSamplesPerBlock == 0 || capacity == 0)
            return 0;

        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        if (numChannels > maximum / maxSamplesPerBlock
            || numChannels * maxSamplesPerBlock > maximum / capacity)
            throw std::length_error ("SampleBlockFifo allocation is too large");

        return numChannels * maxSamplesPerBlock * capacity;
    }

    bool isValidBlock (const float* const* source,
                       std::size_t numChannels,
                       std::size_t numSamples) const noexcept
    {
        if (source == nullptr || numChannels == 0 || numChannels > channelCapacity
            || numSamples == 0 || numSamples > sampleCapacity)
            return false;

        for (std::size_t channel = 0; channel < numChannels; ++channel)
            if (source[channel] == nullptr)
                return false;

        return true;
    }

    const std::size_t channelCapacity;
    const std::size_t sampleCapacity;
    const std::size_t slotCapacity;
    juce::AbstractFifo fifo;
    std::vector<Metadata> metadata;
    std::vector<float> samples;
    std::atomic<std::uint64_t> droppedBlocks { 0 };
    std::atomic<std::uint64_t> droppedSamples { 0 };
    std::atomic<std::uint64_t> rejectedBlocks { 0 };
};
} // namespace spectrumviewer

#endif // SAMPLE_BLOCK_FIFO_H_INCLUDED
