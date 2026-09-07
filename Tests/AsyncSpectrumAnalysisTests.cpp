/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include "gtest/gtest.h"

#include "AsyncSpectrumAnalysis.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{
using spectrumviewer::AsyncSpectrumAnalysis;
using spectrumviewer::PreparedSpectrumAnalysis;
using spectrumviewer::SpectrumAnalysisPreparationRequest;
using spectrumviewer::SpectrumAnalysisPreparationResult;

SpectrumAnalysisPreparationRequest makeRequest (std::uint64_t generation)
{
    SpectrumAnalysisPreparationRequest request;
    request.parameters.channelCount = 1;
    request.parameters.windowSampleCount = 8;
    request.parameters.hopSampleCount = 4;
    request.parameters.maximumInputBlockSampleCount = 4;
    request.parameters.sampleRateHz = 800.0;
    request.parameters.timeHalfBandwidth = 2.0;
    request.parameters.taperCount = 3;
    request.parameters.generation = generation;
    request.sourceChannelIndices = { 7 };
    request.outputQueueCapacity = 3;
    return request;
}

bool waitForResult (AsyncSpectrumAnalysis& analysis,
                    SpectrumAnalysisPreparationResult& result)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (analysis.tryTakeLatest (result))
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

std::shared_ptr<PreparedSpectrumAnalysis> build (
    SpectrumAnalysisPreparationRequest request)
{
    return std::make_shared<PreparedSpectrumAnalysis> (
        request.parameters,
        std::move (request.sourceChannelIndices),
        request.outputQueueCapacity,
        std::move (request.sourceChannelUnits));
}

TEST (AsyncSpectrumAnalysisTests, BuildsACompleteRuntimeOffThread)
{
    AsyncSpectrumAnalysis analysis;
    analysis.request (makeRequest (4));

    SpectrumAnalysisPreparationResult result;
    ASSERT_TRUE (waitForResult (analysis, result));
    ASSERT_TRUE (result.succeeded()) << result.error;
    EXPECT_EQ (result.generation, 4u);
    EXPECT_EQ (result.analysis->getConfiguration().getParameters().generation, 4u);
    EXPECT_EQ (result.analysis->getFrameFifo().getCapacity(), 3u);
}

TEST (AsyncSpectrumAnalysisTests, RequestDoesNotWaitForAnInProgressBuildAndLatestWins)
{
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool firstBuildEntered = false;
    bool releaseFirstBuild = false;

    AsyncSpectrumAnalysis analysis (
        [&] (SpectrumAnalysisPreparationRequest request)
        {
            if (request.parameters.generation == 1)
            {
                std::unique_lock<std::mutex> lock (gateMutex);
                firstBuildEntered = true;
                gateChanged.notify_one();
                gateChanged.wait (lock, [&] { return releaseFirstBuild; });
            }
            return build (std::move (request));
        });

    analysis.request (makeRequest (1));
    {
        std::unique_lock<std::mutex> lock (gateMutex);
        ASSERT_TRUE (gateChanged.wait_for (
            lock, std::chrono::seconds (5), [&] { return firstBuildEntered; }));
    }

    // This would deadlock if the builder held the request mailbox mutex while
    // planning. It must only replace the pending request and return.
    analysis.request (makeRequest (2));
    {
        const std::lock_guard<std::mutex> lock (gateMutex);
        releaseFirstBuild = true;
    }
    gateChanged.notify_one();

    SpectrumAnalysisPreparationResult result;
    ASSERT_TRUE (waitForResult (analysis, result));
    ASSERT_TRUE (result.succeeded()) << result.error;
    EXPECT_EQ (result.generation, 2u);
}

TEST (AsyncSpectrumAnalysisTests, ReportsFailuresAndContinuesServingRequests)
{
    AsyncSpectrumAnalysis analysis (
        [] (SpectrumAnalysisPreparationRequest request)
        {
            if (request.parameters.generation == 8)
                throw std::runtime_error ("deliberate planning failure");
            return build (std::move (request));
        });

    analysis.request (makeRequest (8));
    SpectrumAnalysisPreparationResult failed;
    ASSERT_TRUE (waitForResult (analysis, failed));
    EXPECT_FALSE (failed.succeeded());
    EXPECT_EQ (failed.generation, 8u);
    EXPECT_EQ (failed.error, "deliberate planning failure");

    analysis.request (makeRequest (9));
    SpectrumAnalysisPreparationResult recovered;
    ASSERT_TRUE (waitForResult (analysis, recovered));
    ASSERT_TRUE (recovered.succeeded()) << recovered.error;
    EXPECT_EQ (recovered.generation, 9u);
}
} // namespace
