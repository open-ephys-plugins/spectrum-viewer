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
    std::size_t captureTargetWindowCount)
    : configuration (std::make_shared<const SpectrumAnalysisConfiguration> (parameters)),
      pipeline (configuration),
      displayReducer (parameters.channelCount,
                      configuration->getBinCount(),
                      configuration->getBinCount()),
      frameFifo (parameters.channelCount,
                 configuration->getBinCount(),
                 outputQueueCapacity,
                 configuration->getFrameDescriptor(),
                 std::move (sourceChannelIndices),
                 std::move (sourceChannelUnits)),
      captureAccumulator (captureTargetWindowCount > 0
                              ? std::make_unique<SpectrumCaptureAccumulator> (
                                    parameters.channelCount,
                                    configuration->getBinCount(),
                                    captureTargetWindowCount)
                              : nullptr),
      captureIdentifier (captureId)
{
    if ((captureId == 0) != (captureTargetWindowCount == 0))
        throw std::invalid_argument ("Capture runtime metadata is incomplete");
}

AsyncSpectrumAnalysis::AsyncSpectrumAnalysis (Builder newBuilder)
    : juce::Thread ("Spectrum Viewer Configuration"),
      builder (newBuilder ? std::move (newBuilder) : Builder { buildDefault })
{
    if (! startThread (juce::Thread::Priority::low))
        throw std::runtime_error ("Unable to start Spectrum Viewer configuration thread");
}

AsyncSpectrumAnalysis::~AsyncSpectrumAnalysis()
{
    stopThread (5000);
}

void AsyncSpectrumAnalysis::request (SpectrumAnalysisPreparationRequest request)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        latestRequestedGeneration = request.parameters.generation;
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

void AsyncSpectrumAnalysis::retire (std::shared_ptr<PreparedSpectrumAnalysis> analysis)
{
    if (analysis == nullptr)
        return;

    {
        const std::lock_guard<std::mutex> lock (mutex);
        retired.push_back (std::move (analysis));
    }
    notify();
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
        request.captureTargetWindowCount);
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
