/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "AsyncSpectrumAnalysis.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace spectrumviewer
{
PreparedSpectrumAnalysis::PreparedSpectrumAnalysis (
    SpectrumAnalysisParameters parameters,
    std::vector<int> sourceChannelIndices,
    std::size_t outputQueueCapacity,
    std::vector<std::string> sourceChannelUnits,
    std::uint64_t captureId,
    std::size_t captureTargetWindowCount,
    std::uint16_t sourceStreamId,
    std::vector<int> sourceGlobalChannelIndices)
    : configuration (std::make_shared<const SpectrumAnalysisConfiguration> (parameters)),
      pipeline (configuration),
      displayReducer (parameters.channelCount,
                      configuration->getBinCount(),
                      configuration->getBinCount()),
      referenceDisplayReducer (parameters.channelCount,
                               configuration->getBinCount(),
                               configuration->getBinCount()),
      comparisonScratch (parameters.channelCount * configuration->getBinCount()),
      baselineEstimator (configuration->getBinCount(),
                         configuration->getBinCount()),
      baselineScratch (parameters.channelCount * configuration->getBinCount()),
      frameFifo (parameters.channelCount,
                 configuration->getBinCount(),
                 outputQueueCapacity,
                 configuration->getFrameDescriptor(),
                 std::move (sourceChannelIndices),
                 std::move (sourceChannelUnits),
                 sourceStreamId),
      captureAccumulator (captureTargetWindowCount > 0
                              ? std::make_unique<SpectrumCaptureAccumulator> (
                                    parameters.channelCount,
                                    configuration->getBinCount(),
                                    captureTargetWindowCount)
                              : nullptr),
      captureIdentifier (captureId),
      inputStreamId (sourceStreamId),
      globalChannelIndices (std::move (sourceGlobalChannelIndices))
{
    if ((captureId == 0) != (captureTargetWindowCount == 0))
        throw std::invalid_argument ("Capture runtime metadata is incomplete");
    if (globalChannelIndices.size() != parameters.channelCount
        || std::any_of (globalChannelIndices.begin(),
                        globalChannelIndices.end(),
                        [] (int channel) { return channel < 0; }))
        throw std::invalid_argument ("Spectrum input routing metadata is invalid");
}

AsyncSpectrumAnalysis::AsyncSpectrumAnalysis (Builder newBuilder)
    : juce::Thread ("Spectrum Viewer Configuration"),
      builder (newBuilder ? std::move (newBuilder) : Builder { buildDefault })
{
    // This object is a direct member of SpectrumViewer, so throwing here would
    // escape Plugin::createProcessor across the plugin ABI and take the host
    // down. Degrade instead: request() reports the failure through the normal
    // result path, which the worker already handles as configurationFailed.
    configurationThreadAvailable = startThread (juce::Thread::Priority::low);

    // retire() must be noexcept because it runs from ~SpectrumViewer and from
    // inside Thread::run(). Reserve enough room that the common case never
    // reallocates: at most a handful of runtimes are ever in flight.
    try
    {
        retired.reserve (retiredCapacityHint);
    }
    catch (const std::bad_alloc&)
    {
        // Not fatal; retire() falls back to destroying on the calling thread.
    }
}

AsyncSpectrumAnalysis::~AsyncSpectrumAnalysis()
{
    signalThreadShouldExit();
    notify();
    waitForThreadToExit (-1);
}

void AsyncSpectrumAnalysis::request (SpectrumAnalysisPreparationRequest request)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        latestRequestedGeneration = request.parameters.generation;

        if (! configurationThreadAvailable)
        {
            // No thread will ever consume this. Publish the failure directly so
            // the worker adopts it, counts it, and reports configurationFailed
            // instead of waiting forever on a preparation that cannot happen.
            SpectrumAnalysisPreparationResult failure;
            failure.generation = request.parameters.generation;
            failure.captureId = request.captureId;
            failure.error = "Spectrum Viewer configuration thread is unavailable";
            completed = std::move (failure);
            pending.reset();
            return;
        }

        pending = std::move (request);
    }
    notify();
}

bool AsyncSpectrumAnalysis::tryTakeLatest (SpectrumAnalysisPreparationResult& result)
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (! completed.has_value())
        return false;

    result = std::move (*completed);
    completed.reset();
    return true;
}

void AsyncSpectrumAnalysis::retire (std::shared_ptr<PreparedSpectrumAnalysis> analysis) noexcept
{
    if (analysis == nullptr)
        return;

    // Callers include ~SpectrumViewer and SpectrumViewer::run(); an exception
    // escaping either is std::terminate. Never propagate from here.
    try
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            retired.push_back (std::move (analysis));
        }
        notify();
        return;
    }
    catch (...)
    {
        // Fall through.
    }

    // Handing the runtime to the configuration thread failed. Destroying it on
    // the calling thread takes the FFTW planner mutex here instead, which is
    // undesirable but strictly better than terminating. Never reached from the
    // audio callback, which does not retire runtimes.
    analysis.reset();
}

std::shared_ptr<PreparedSpectrumAnalysis> AsyncSpectrumAnalysis::buildDefault (
    SpectrumAnalysisPreparationRequest request)
{
    return std::make_shared<PreparedSpectrumAnalysis> (
        request.parameters,
        std::move (request.sourceChannelIndices),
        request.outputQueueCapacity,
        std::move (request.sourceChannelUnits),
        request.captureId,
        request.captureTargetWindowCount,
        request.sourceStreamId,
        std::move (request.sourceGlobalChannelIndices));
}

void AsyncSpectrumAnalysis::run()
{
    for (;;)
    {
        std::optional<SpectrumAnalysisPreparationRequest> request;
        std::vector<std::shared_ptr<PreparedSpectrumAnalysis>> destroyHere;
        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (threadShouldExit())
            {
                pending.reset();
                completed.reset();
                destroyHere.swap (retired);
                return;
            }

            if (completed.has_value()
                && completed->generation != latestRequestedGeneration)
            {
                if (completed->analysis != nullptr)
                    destroyHere.push_back (std::move (completed->analysis));
                completed.reset();
            }

            auto retainedEnd = std::remove_if (
                retired.begin(),
                retired.end(),
                [&destroyHere] (const auto& analysis)
                {
                    if (analysis.use_count() != 1)
                        return false;
                    destroyHere.push_back (analysis);
                    return true;
                });
            retired.erase (retainedEnd, retired.end());

            if (pending.has_value())
            {
                request = std::move (pending);
                pending.reset();
            }
        }

        // Destruction and every expensive build operation happen without the
        // mailbox mutex, and only on this configuration thread.
        destroyHere.clear();
        if (! request.has_value())
        {
            wait (10);
            continue;
        }

        SpectrumAnalysisPreparationResult result;
        result.generation = request->parameters.generation;
        result.captureId = request->captureId;
        try
        {
            result.analysis = builder (std::move (*request));
            if (result.analysis == nullptr)
                result.error = "Spectrum analysis builder returned no runtime";
        }
        catch (const std::exception& error)
        {
            result.error = error.what();
        }
        catch (...)
        {
            result.error = "Unknown spectrum analysis configuration failure";
        }

        {
            const std::lock_guard<std::mutex> lock (mutex);
            if (result.generation == latestRequestedGeneration && ! pending.has_value())
            {
                completed = std::move (result);
            }
            // Otherwise result is deliberately destroyed here, on this thread.
        }
    }
}
} // namespace spectrumviewer
