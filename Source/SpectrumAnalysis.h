/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef SPECTRUM_ANALYSIS_H_INCLUDED
#define SPECTRUM_ANALYSIS_H_INCLUDED

#include "MultitaperPeriodogram.h"
#include "SampleWindowAssembler.h"
#include "SpectrumEstimation.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace spectrumviewer
{
struct SpectrumAnalysisParameters
{
    std::size_t channelCount = 0;
    std::size_t windowSampleCount = 0;
    std::size_t hopSampleCount = 0;
    std::size_t maximumInputBlockSampleCount = 0;
    double sampleRateHz = 0.0;
    double timeHalfBandwidth = 0.0;
    std::size_t taperCount = 0;
    DetrendMode detrendMode = DetrendMode::mean;
    std::uint64_t generation = 0;
    unsigned int dpssPlanningFlags = 1U << 6U; // FFTW_ESTIMATE
    unsigned int estimatorPlanningFlags = 1U << 6U; // FFTW_ESTIMATE
};

/**
    Validated immutable configuration and precomputed DPSS bank.

    Construct this object before starting the analysis or audio threads. DPSS
    generation and all allocations happen in the constructor.
*/
class SpectrumAnalysisConfiguration
{
public:
    explicit SpectrumAnalysisConfiguration (SpectrumAnalysisParameters parameters);

    const SpectrumAnalysisParameters& getParameters() const noexcept { return values; }
    const SpectrumFrameDescriptor& getFrameDescriptor() const noexcept { return descriptor; }
    const std::shared_ptr<const DpssTaperBank>& getTaperBank() const noexcept { return taperBank; }
    std::size_t getBinCount() const noexcept { return values.windowSampleCount / 2 + 1; }

private:
    SpectrumAnalysisParameters values;
    SpectrumFrameDescriptor descriptor;
    std::shared_ptr<const DpssTaperBank> taperBank;
};

/** Worker-owned, allocation-free window assembly and spectral estimation path. */
class SpectrumAnalysisPipeline
{
public:
    enum class AppendStatus
    {
        accepted,
        invalidBlock,
        configurationMismatch
    };

    struct AppendResult
    {
        std::size_t windowsReady = 0;
        bool discontinuity = false;
        AppendStatus status = AppendStatus::invalidBlock;
    };

    class FrameView
    {
    public:
        const float* getChannelData (std::size_t channel) const noexcept
        {
            return channel < numChannels ? data + channel * numBins : nullptr;
        }

        SpectrumFrameDescriptor descriptor;
        std::int64_t firstSample = 0;
        std::uint64_t sequence = 0;
        std::size_t numChannels = 0;
        std::size_t numBins = 0;

    private:
        friend class SpectrumAnalysisPipeline;
        const float* data = nullptr;
    };

    explicit SpectrumAnalysisPipeline (
        std::shared_ptr<const SpectrumAnalysisConfiguration> configuration);

    AppendResult appendBlock (const float* const* source,
                              std::size_t numChannels,
                              std::size_t numSamples,
                              std::int64_t firstSample,
                              std::uint64_t configurationGeneration);

    template <typename Consumer>
    std::size_t consumeReadyFrames (Consumer&& consumer)
    {
        std::size_t completed = 0;
        const auto processWindow = [&] (const auto& window)
        {
            const auto sequence = nextFrameSequence++;
            for (std::size_t channel = 0; channel < window.numChannels; ++channel)
            {
                const auto source = window.getChannel (channel);
                channelViews[channel] = { source.firstData,
                                          source.firstSize,
                                          source.secondData,
                                          source.secondSize };
            }

            if (! estimator.compute (channelViews.data(), channelViews.size()))
            {
                ++failedWindowCount;
                return;
            }

            FrameView frame;
            frame.data = estimator.getChannelData (0);
            frame.descriptor = configuration->getFrameDescriptor();
            frame.firstSample = window.firstSample;
            frame.sequence = sequence;
            frame.numChannels = window.numChannels;
            frame.numBins = estimator.getBinCount();
            consumer (frame);
            ++completed;
        };
        assembler.consumeReadyWindows (processWindow);
        return completed;
    }

    void reset() noexcept { assembler.reset(); }
    const SpectrumAnalysisConfiguration& getConfiguration() const noexcept { return *configuration; }
    std::uint64_t getFailedWindowCount() const noexcept { return failedWindowCount; }
    std::uint64_t getDiscontinuityCount() const noexcept { return assembler.getDiscontinuityCount(); }
    std::size_t getBufferedSampleCount() const noexcept { return assembler.getBufferedSampleCount(); }

private:
    std::shared_ptr<const SpectrumAnalysisConfiguration> configuration;
    SampleWindowAssembler assembler;
    MultitaperPeriodogram estimator;
    std::vector<ChannelSampleView> channelViews;
    std::uint64_t nextFrameSequence = 0;
    std::uint64_t failedWindowCount = 0;
};
} // namespace spectrumviewer

#endif // SPECTRUM_ANALYSIS_H_INCLUDED
