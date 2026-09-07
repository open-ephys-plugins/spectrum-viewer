/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "AsyncSpectrumAnalysis.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace spectrumviewer
{
PreparedSpectrumAnalysis::PreparedSpectrumAnalysis (
    SpectrumAnalysisParameters parameters,
    std::vector<int> sourceChannelIndices,
    std::size_t outputQueueCapacity)
    : configuration (std::make_shared<const SpectrumAnalysisConfiguration> (parameters)),
      pipeline (configuration),
      frameFifo (parameters.channelCount,
                 configuration->getBinCount(),
                 outputQueueCapacity,
                 configuration->getFrameDescriptor(),
                 std::move (sourceChannelIndices))
{
}

AsyncSpectrumAnalysis::AsyncSpectrumAnalysis (Builder newBuilder)
    : builder (newBuilder ? std::move (newBuilder) : Builder { buildDefault }),
      thread ([this] { run(); })
{
}

AsyncSpectrumAnalysis::~AsyncSpectrumAnalysis()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        shouldExit = true;
    }
    wake.notify_one();
    if (thread.joinable())
        thread.join();
}

void AsyncSpectrumAnalysis::request (SpectrumAnalysisPreparationRequest request)
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        latestRequestedGeneration = request.parameters.generation;
        pending = std::move (request);
    }
    wake.notify_one();
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
    wake.notify_one();
}

std::shared_ptr<PreparedSpectrumAnalysis> AsyncSpectrumAnalysis::buildDefault (
    SpectrumAnalysisPreparationRequest request)
{
    return std::make_shared<PreparedSpectrumAnalysis> (
        request.parameters,
        std::move (request.sourceChannelIndices),
        request.outputQueueCapacity);
}

void AsyncSpectrumAnalysis::run()
{
    for (;;)
    {
        std::optional<SpectrumAnalysisPreparationRequest> request;
        std::vector<std::shared_ptr<PreparedSpectrumAnalysis>> destroyHere;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait_for (lock,
                           std::chrono::milliseconds (10),
                           [this] { return shouldExit || pending.has_value(); });

            if (shouldExit)
            {
                pending.reset();
                completed.reset();
                destroyHere.swap (retired);
                lock.unlock();
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
            continue;

        SpectrumAnalysisPreparationResult result;
        result.generation = request->parameters.generation;
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
