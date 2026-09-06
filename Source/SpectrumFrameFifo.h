/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ------------------------------------------------------------------
*/

#ifndef SPECTRUM_FRAME_FIFO_H_INCLUDED
#define SPECTRUM_FRAME_FIFO_H_INCLUDED

#include <AppConfig.h>
#include <juce_core/juce_core.h>

#include "SpectrumEstimation.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace spectrumviewer
{
/**
    Publishes complete planar spectrum frames from one worker to one UI thread.

    Storage is fixed at construction. The producer drops a complete frame when the
    queue is full. The consumer discards queued stale frames and observes only the
    newest complete frame available at the start of each drain.

    Exactly one thread may call tryPush(), and exactly one other thread may call
    tryPopLatest(). reset() is only safe while both threads are stopped.
*/
class SpectrumFrameFifo
{
public:
    class FrameView
    {
    public:
        const float* getChannelData (std::size_t channel) const noexcept
        {
            return channel < numChannels ? data + channel * binStride : nullptr;
        }

        int getSourceChannelIndex (std::size_t channel) const noexcept
        {
            return channel < numChannels ? sourceChannelIndices[channel] : -1;
        }

        std::int64_t firstSample = 0;
        std::uint64_t configurationGeneration = 0;
        std::uint64_t sequence = 0;
        std::size_t numChannels = 0;
        std::size_t numBins = 0;
        SpectrumFrameDescriptor descriptor;

    private:
        friend class SpectrumFrameFifo;
        const float* data = nullptr;
        const int* sourceChannelIndices = nullptr;
        std::size_t binStride = 0;
    };

    SpectrumFrameFifo (std::size_t numChannels,
                       std::size_t numBins,
                       std::size_t capacity,
                       SpectrumFrameDescriptor frameDescriptor)
        : SpectrumFrameFifo (numChannels,
                             numBins,
                             capacity,
                             frameDescriptor,
                             sequentialChannelIndices (numChannels))
    {
    }

    SpectrumFrameFifo (std::size_t numChannels,
                       std::size_t numBins,
                       std::size_t capacity,
                       SpectrumFrameDescriptor frameDescriptor,
                       std::vector<int> sourceChannels)
        : channelCount (numChannels),
          binCount (numBins),
          slotCapacity (capacity),
          descriptor (frameDescriptor),
          sourceChannelIndices (std::move (sourceChannels)),
          fifo (checkedFifoSize (capacity)),
          metadata (capacity + 1),
          powers (checkedPowerCount (numChannels, numBins, capacity + 1))
    {
        if (numChannels == 0 || numBins == 0
            || sourceChannelIndices.size() != numChannels
            || ! descriptorIsValid (descriptor, numBins))
            throw std::invalid_argument ("SpectrumFrameFifo configuration is invalid");
    }

    bool tryPush (const float* planarPowers,
                  std::size_t numChannels,
                  std::size_t numBins,
                  std::int64_t firstSample,
                  std::uint64_t sequence) noexcept
    {
        if (planarPowers == nullptr || numChannels != channelCount || numBins != binCount)
        {
            rejectedFrames.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        auto writer = fifo.write (1);
        if (writer.blockSize1 + writer.blockSize2 != 1)
        {
            droppedFrames.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const auto slot = static_cast<std::size_t> (writer.startIndex1);
        std::memcpy (powers.data() + slot * channelCount * binCount,
                     planarPowers,
                     channelCount * binCount * sizeof (float));
        metadata[slot] = { firstSample, sequence };
        return true;
    }

    template <typename Consumer>
    bool tryPopLatest (Consumer&& consumer)
    {
        const auto ready = fifo.getNumReady();
        if (ready <= 0)
            return false;

        auto reader = fifo.read (ready);
        const auto consumed = reader.blockSize1 + reader.blockSize2;
        if (consumed <= 0)
            return false;

        const auto slot = static_cast<std::size_t> (
            reader.blockSize2 > 0 ? reader.startIndex2 + reader.blockSize2 - 1
                                  : reader.startIndex1 + reader.blockSize1 - 1);
        const auto& frameMetadata = metadata[slot];

        FrameView view;
        view.data = powers.data() + slot * channelCount * binCount;
        view.sourceChannelIndices = sourceChannelIndices.data();
        view.binStride = binCount;
        view.firstSample = frameMetadata.firstSample;
        view.configurationGeneration = descriptor.configurationGeneration;
        view.sequence = frameMetadata.sequence;
        view.numChannels = channelCount;
        view.numBins = binCount;
        view.descriptor = descriptor;
        consumer (view);

        staleFrames.fetch_add (static_cast<std::uint64_t> (consumed - 1), std::memory_order_relaxed);
        return true;
    }

    /** Clears queued frames and counters. Call only while producer and consumer are stopped. */
    void reset() noexcept
    {
        fifo.reset();
        droppedFrames.store (0, std::memory_order_relaxed);
        rejectedFrames.store (0, std::memory_order_relaxed);
        staleFrames.store (0, std::memory_order_relaxed);
    }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getBinCount() const noexcept { return binCount; }
    std::size_t getCapacity() const noexcept { return slotCapacity; }
    std::size_t getNumReady() const noexcept { return static_cast<std::size_t> (fifo.getNumReady()); }
    std::uint64_t getDroppedFrameCount() const noexcept { return droppedFrames.load (std::memory_order_relaxed); }
    std::uint64_t getRejectedFrameCount() const noexcept { return rejectedFrames.load (std::memory_order_relaxed); }
    std::uint64_t getStaleFrameCount() const noexcept { return staleFrames.load (std::memory_order_relaxed); }

private:
    struct Metadata
    {
        std::int64_t firstSample = 0;
        std::uint64_t sequence = 0;
    };

    static bool descriptorIsValid (const SpectrumFrameDescriptor& value,
                                   std::size_t numBins) noexcept
    {
        const auto validDetrendMode = value.detrendMode == DetrendMode::none
                                      || value.detrendMode == DetrendMode::mean
                                      || value.detrendMode == DetrendMode::linear;
        return std::isfinite (value.sampleRateHz) && value.sampleRateHz > 0.0
               && std::isfinite (value.binWidthHz) && value.binWidthHz > 0.0
               && std::isfinite (value.timeHalfBandwidth) && value.timeHalfBandwidth > 0.0
               && value.windowSampleCount > 0 && value.hopSampleCount > 0
               && value.taperCount > 0
               && value.timeHalfBandwidth < 0.5 * static_cast<double> (value.windowSampleCount)
               && value.binWidthHz
                      == value.sampleRateHz / static_cast<double> (value.windowSampleCount)
               && validDetrendMode
               && value.valueKind == SpectrumValueKind::powerSpectralDensity
               && numBins == value.windowSampleCount / 2 + 1;
    }

    static std::vector<int> sequentialChannelIndices (std::size_t count)
    {
        std::vector<int> result (count);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (index > static_cast<std::size_t> (std::numeric_limits<int>::max()))
                throw std::invalid_argument ("SpectrumFrameFifo channel count is out of range");
            result[index] = static_cast<int> (index);
        }
        return result;
    }

    static int checkedFifoSize (std::size_t capacity)
    {
        if (capacity == 0 || capacity >= static_cast<std::size_t> (std::numeric_limits<int>::max()))
            throw std::invalid_argument ("SpectrumFrameFifo capacity is out of range");
        return static_cast<int> (capacity + 1);
    }

    static std::size_t checkedPowerCount (std::size_t numChannels,
                                          std::size_t numBins,
                                          std::size_t capacity)
    {
        if (numChannels == 0 || numBins == 0 || capacity == 0)
            return 0;

        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        if (numChannels > maximum / numBins || numChannels * numBins > maximum / capacity)
            throw std::length_error ("SpectrumFrameFifo allocation is too large");
        return numChannels * numBins * capacity;
    }

    const std::size_t channelCount;
    const std::size_t binCount;
    const std::size_t slotCapacity;
    const SpectrumFrameDescriptor descriptor;
    const std::vector<int> sourceChannelIndices;
    juce::AbstractFifo fifo;
    std::vector<Metadata> metadata;
    std::vector<float> powers;
    std::atomic<std::uint64_t> droppedFrames { 0 };
    std::atomic<std::uint64_t> rejectedFrames { 0 };
    std::atomic<std::uint64_t> staleFrames { 0 };
};
} // namespace spectrumviewer

#endif // SPECTRUM_FRAME_FIFO_H_INCLUDED
