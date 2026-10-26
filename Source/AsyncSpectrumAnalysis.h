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

#include "AperiodicSpectrumBaseline.h"
#include "SpectrumAnalysis.h"
#include "SpectrumCaptureAccumulator.h"
#include "SpectrumFrameFifo.h"

#include <AppConfig.h>
#include <TestableExport.h>
#include <juce_core/juce_core.h>

#include <cstddef>
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
class TESTABLE PreparedSpectrumAnalysis
{
public:
    PreparedSpectrumAnalysis (SpectrumAnalysisParameters parameters,
                              std::vector<int> sourceChannelIndices,
                              std::size_t outputQueueCapacity,
                              std::vector<std::string> sourceChannelUnits = {},
                              std::uint64_t captureId = 0,
                              std::size_t captureTargetWindowCount = 0,
                              std::uint16_t sourceStreamId = 0,
                              std::vector<int> sourceGlobalChannelIndices = {});

    SpectrumAnalysisPipeline& getPipeline() noexcept { return pipeline; }
    SpectrumFrameFifo& getFrameFifo() noexcept { return frameFifo; }
    SpectrumDisplayReducer& getDisplayReducer() noexcept { return displayReducer; }
    SpectrumDisplayReducer& getReferenceDisplayReducer() noexcept { return referenceDisplayReducer; }
    float* getComparisonScratch() noexcept { return comparisonScratch.data(); }
    AperiodicSpectrumBaseline& getBaselineEstimator() noexcept { return baselineEstimator; }
    float* getBaselineScratch() noexcept { return baselineScratch.data(); }
    const SpectrumAnalysisConfiguration& getConfiguration() const noexcept { return *configuration; }
    SpectrumCaptureAccumulator* getCaptureAccumulator() noexcept { return captureAccumulator.get(); }
    bool isCaptureRuntime() const noexcept { return captureAccumulator != nullptr; }
    std::uint64_t getCaptureId() const noexcept { return captureIdentifier; }
    std::uint16_t getSourceStreamId() const noexcept { return inputStreamId; }
    const std::vector<int>& getSourceGlobalChannelIndices() const noexcept
    {
        return globalChannelIndices;
    }

private:
    std::shared_ptr<const SpectrumAnalysisConfiguration> configuration;
    SpectrumAnalysisPipeline pipeline;
    SpectrumDisplayReducer displayReducer;
    SpectrumDisplayReducer referenceDisplayReducer;
    std::vector<float> comparisonScratch;
    AperiodicSpectrumBaseline baselineEstimator;
    std::vector<float> baselineScratch;
    SpectrumFrameFifo frameFifo;
    std::unique_ptr<SpectrumCaptureAccumulator> captureAccumulator;
    std::uint64_t captureIdentifier = 0;
    std::uint16_t inputStreamId = 0;
    std::vector<int> globalChannelIndices;
};

struct SpectrumAnalysisPreparationRequest
{
    SpectrumAnalysisParameters parameters;
    std::vector<int> sourceChannelIndices;
    std::vector<std::string> sourceChannelUnits;
    std::uint16_t sourceStreamId = 0;
    std::vector<int> sourceGlobalChannelIndices;
    std::size_t outputQueueCapacity = 0;
    std::uint64_t captureId = 0;
    std::size_t captureTargetWindowCount = 0;
};

struct SpectrumAnalysisPreparationResult
{
    std::uint64_t generation = 0;
    std::uint64_t captureId = 0;
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

    /** Hands a runtime to the configuration thread for destruction.

        Safe to call from a destructor or from Thread::run(): it never throws.
    */
    void retire (std::shared_ptr<PreparedSpectrumAnalysis> analysis) noexcept;

    /** False when the configuration thread could not be started. */
    bool isConfigurationThreadAvailable() const noexcept
    {
        return configurationThreadAvailable;
    }

private:
    void run() override;
    static std::shared_ptr<PreparedSpectrumAnalysis> buildDefault (
        SpectrumAnalysisPreparationRequest request);

    // At most a handful of runtimes are ever awaiting destruction: the active
    // one, a replacement, and a superseded preparation.
    static constexpr std::size_t retiredCapacityHint = 8;

    bool configurationThreadAvailable = false;
    Builder builder;
    std::mutex mutex;
    std::optional<SpectrumAnalysisPreparationRequest> pending;
    std::optional<SpectrumAnalysisPreparationResult> completed;
    std::vector<std::shared_ptr<PreparedSpectrumAnalysis>> retired;
    std::uint64_t latestRequestedGeneration = 0;
};
} // namespace spectrumviewer

#endif // ASYNC_SPECTRUM_ANALYSIS_H_INCLUDED
