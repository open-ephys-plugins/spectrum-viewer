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

#ifndef ASYNC_SPECTRUM_ANALYSIS_H_INCLUDED
#define ASYNC_SPECTRUM_ANALYSIS_H_INCLUDED

#include "SpectrumAnalysis.h"
#include "SpectrumFrameFifo.h"

#include <AppConfig.h>
#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace spectrumviewer
{
/** Everything the analysis worker needs after an allocation-free activation. */
class PreparedSpectrumAnalysis
{
public:
    PreparedSpectrumAnalysis (SpectrumAnalysisParameters parameters,
                              std::vector<int> sourceChannelIndices,
                              std::size_t outputQueueCapacity);

    SpectrumAnalysisPipeline& getPipeline() noexcept { return pipeline; }
    SpectrumFrameFifo& getFrameFifo() noexcept { return frameFifo; }
    const SpectrumAnalysisConfiguration& getConfiguration() const noexcept { return *configuration; }

private:
    std::shared_ptr<const SpectrumAnalysisConfiguration> configuration;
    SpectrumAnalysisPipeline pipeline;
    SpectrumFrameFifo frameFifo;
};

struct SpectrumAnalysisPreparationRequest
{
    SpectrumAnalysisParameters parameters;
    std::vector<int> sourceChannelIndices;
    std::size_t outputQueueCapacity = 0;
};

struct SpectrumAnalysisPreparationResult
{
    std::uint64_t generation = 0;
    std::shared_ptr<PreparedSpectrumAnalysis> analysis;
    std::string error;

    bool succeeded() const noexcept { return analysis != nullptr; }
};

/**
    Coalescing, non-real-time builder for complete analysis runtimes.

    request() only moves a small request under a mutex; the mutex is never held
    while DPSS generation, allocation, or FFTW planning runs. The audio callback
    must not call any method on this class. Superseded and retired runtimes are
    destroyed on the configuration thread.
*/
class AsyncSpectrumAnalysis : private juce::Thread
{
public:
    using Builder = std::function<std::shared_ptr<PreparedSpectrumAnalysis> (
        SpectrumAnalysisPreparationRequest)>;

    explicit AsyncSpectrumAnalysis (Builder builder = {});
    ~AsyncSpectrumAnalysis();

    AsyncSpectrumAnalysis (const AsyncSpectrumAnalysis&) = delete;
    AsyncSpectrumAnalysis& operator= (const AsyncSpectrumAnalysis&) = delete;

    void request (SpectrumAnalysisPreparationRequest request);
    bool tryTakeLatest (SpectrumAnalysisPreparationResult& result);
    void retire (std::shared_ptr<PreparedSpectrumAnalysis> analysis);

private:
    void run() override;
    static std::shared_ptr<PreparedSpectrumAnalysis> buildDefault (
        SpectrumAnalysisPreparationRequest request);

    Builder builder;
    std::mutex mutex;
    std::optional<SpectrumAnalysisPreparationRequest> pending;
    std::optional<SpectrumAnalysisPreparationResult> completed;
    std::vector<std::shared_ptr<PreparedSpectrumAnalysis>> retired;
    std::uint64_t latestRequestedGeneration = 0;
};
} // namespace spectrumviewer

#endif // ASYNC_SPECTRUM_ANALYSIS_H_INCLUDED
