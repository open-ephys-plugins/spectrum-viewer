/* Processor-level tests for asynchronous Spectrum Viewer configuration. */

#include "gtest/gtest.h"

#include "SpectrumViewer.h"
#include <TestFixtures.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
using Request = spectrumviewer::SpectrumAnalysisPreparationRequest;
using Runtime = spectrumviewer::PreparedSpectrumAnalysis;
using namespace std::chrono_literals;

std::shared_ptr<Runtime> buildRuntime (Request request)
{
    return std::make_shared<Runtime> (request.parameters,
                                      std::move (request.sourceChannelIndices),
                                      request.outputQueueCapacity,
                                      std::move (request.sourceChannelUnits),
                                      request.captureId,
                                      request.captureTargetWindowCount,
                                      request.sourceStreamId,
                                      std::move (request.sourceGlobalChannelIndices));
}

struct BuildGate
{
    ~BuildGate() { release(); }

    void enterAndWait()
    {
        std::unique_lock<std::mutex> lock (mutex);
        entered = true;
        changed.notify_all();
        changed.wait (lock, [this] { return released; });
    }

    bool waitUntilEntered()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, 5s, [this] { return entered; });
    }

    void release()
    {
        {
            const std::lock_guard<std::mutex> lock (mutex);
            released = true;
        }
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

class SpectrumViewerLifecycleTests : public testing::Test
{
protected:
    static constexpr float sampleRate = 80.0f;
    static constexpr int blockSize = 5;

    void createProcessor (spectrumviewer::AsyncSpectrumAnalysis::Builder builder = {},
                          int sourceChannelCount = 1,
                          int sourceStreamCount = 1)
    {
        tester = std::make_unique<ProcessorTester> (
            TestSourceNodeBuilder (FakeSourceNodeParams {
                sourceChannelCount, sampleRate, 1.0f, sourceStreamCount }));
        processor = tester->createProcessor<SpectrumViewer> (
            Plugin::Processor::SINK, std::move (builder));
        processor->setRateAndBufferSizeDetails (sampleRate, blockSize);
    }

    void TearDown() override
    {
        if (processor != nullptr
            && processor->getAnalysisReadiness() != SpectrumAnalysisReadiness::stopped)
            processor->stopAcquisition();
        processor = nullptr;
        tester.reset();
    }

    bool waitUntil (const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for (1ms);
        }
        return predicate();
    }

    void writeBlocks (int count)
    {
        writeBlocksScaled (count, 1.0f);
    }

    void writeBlocksScaled (int count, float scale)
    {
        AudioBuffer<float> buffer (1, blockSize);
        for (int sample = 0; sample < blockSize; ++sample)
            buffer.setSample (0, sample, scale * static_cast<float> (sample + 1));

        for (int block = 0; block < count; ++block)
        {
            tester->processBlock (processor, buffer);
            std::this_thread::sleep_for (1ms);
        }
    }

    void writeBlocksWithValue (int count, float value)
    {
        AudioBuffer<float> buffer (1, blockSize);
        buffer.clear();
        for (int sample = 0; sample < blockSize; ++sample)
            buffer.setSample (0, sample, value);
        for (int block = 0; block < count; ++block)
        {
            tester->processBlock (processor, buffer);
            std::this_thread::sleep_for (1ms);
        }
    }

    std::size_t consumeWindowSize()
    {
        std::size_t result = 0;
        processor->consumeLatestSpectrumFrame (
            [&result] (const auto& frame) { result = frame.descriptor.windowSampleCount; });
        return result;
    }

    std::uint16_t consumeStreamId()
    {
        auto result = std::numeric_limits<std::uint16_t>::max();
        processor->consumeLatestSpectrumFrame (
            [&result] (const auto& frame) { result = frame.sourceStreamId; });
        return result;
    }

    std::unique_ptr<ProcessorTester> tester;
    SpectrumViewer* processor = nullptr;
};

TEST_F (SpectrumViewerLifecycleTests, StartPreparesWarmsAndBecomesLive)
{
    auto gate = std::make_shared<BuildGate>();
    createProcessor ([gate] (Request request)
    {
        gate->enterAndWait();
        return buildRuntime (std::move (request));
    });

    ASSERT_TRUE (processor->startAcquisition());
    const auto buildEntered = gate->waitUntilEntered();
    if (! buildEntered)
        gate->release();
    ASSERT_TRUE (buildEntered);
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::preparing);
    EXPECT_TRUE (processor->isAnalysisConfigurationPending());

    writeBlocks (1);
    EXPECT_EQ (processor->getUnconfiguredInputBlockCount(), 1u);
    EXPECT_EQ (processor->getUnconfiguredInputSampleCount(), 5u);

    gate->release();
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::warmingUp;
    }));
    EXPECT_EQ (processor->getWarmupTargetSampleCount(), 20u);

    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    ASSERT_TRUE (waitUntil ([this] { return consumeWindowSize() == 20u; }));
}

TEST_F (SpectrumViewerLifecycleTests, FirstStartResolvesLoadedStreamSelection)
{
    auto requestedStream = std::make_shared<std::atomic<std::uint16_t>> (0);
    createProcessor (
        [requestedStream] (Request request)
        {
            requestedStream->store (request.sourceStreamId);
            return buildRuntime (std::move (request));
        },
        1,
        2);

    const auto secondStream = processor->getDataStreams()[1]->getStreamId();
    auto* streamParameter = dynamic_cast<SelectedStreamParameter*> (
        processor->getParameter ("active_stream"));
    ASSERT_NE (streamParameter, nullptr);
    XmlElement savedParameters ("PARAMETERS");
    savedParameters.setAttribute ("active_stream", 1);
    streamParameter->fromXml (&savedParameters);

    // Loading changes the stored value directly. updateSettings(), rather
    // than a UI callback, must establish the route used by the first start.
    processor->updateSettings();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    EXPECT_EQ (requestedStream->load(), secondStream);
}

TEST_F (SpectrumViewerLifecycleTests, FirstStartDoesNotRequirePreparedHostBlockSize)
{
    createProcessor();
    processor->setRateAndBufferSizeDetails (sampleRate, 0);

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness()
               == SpectrumAnalysisReadiness::live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, NominalJuceBlockSizeDoesNotLimitStreamPayload)
{
    constexpr int nominalJuceBlockSize = 128;
    constexpr int streamPayloadSamples = 696;
    createProcessor();
    processor->setRateAndBufferSizeDetails (sampleRate, nominalJuceBlockSize);

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    AudioBuffer<float> buffer (1, streamPayloadSamples);
    buffer.clear();
    tester->processBlock (processor, buffer);

    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    EXPECT_EQ (processor->getRejectedInputBlockCount(), 0u);
}

TEST_F (SpectrumViewerLifecycleTests, FirstStartDoesNotRequirePopulatedStreamNames)
{
    createProcessor();
    auto* streamParameter = dynamic_cast<SelectedStreamParameter*> (
        processor->getParameter ("active_stream"));
    ASSERT_NE (streamParameter, nullptr);
    streamParameter->setStreamNames ({});

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness()
               == SpectrumAnalysisReadiness::live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, InvalidChannelNamesUseSafeFallbacks)
{
    createProcessor();

    EXPECT_EQ (processor->getChanName (1), "Channel 2");
    EXPECT_EQ (processor->getChanName (100), "Channel 101");
}

TEST_F (SpectrumViewerLifecycleTests, IncompleteHostBufferRejectsWholeInputBlock)
{
    createProcessor ({}, 2);
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    AudioBuffer<float> incompleteBuffer (1, blockSize);
    incompleteBuffer.clear();
    tester->processBlock (processor, incompleteBuffer);

    EXPECT_EQ (processor->getRejectedInputBlockCount(), 1u);
    EXPECT_EQ (processor->getDroppedInputBlockCount(), 0u);
    EXPECT_EQ (processor->getWarmupSampleCount(), 0u);
}

TEST_F (SpectrumViewerLifecycleTests, StartFailsWithoutReplacingAnActiveWorker)
{
    createProcessor();
    ASSERT_TRUE (processor->startThread (juce::Thread::Priority::normal));

    EXPECT_FALSE (processor->startAcquisition());
    EXPECT_EQ (processor->getAnalysisReadiness(),
               SpectrumAnalysisReadiness::configurationFailed);

    EXPECT_TRUE (processor->stopAcquisition());
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::stopped);
}

TEST_F (SpectrumViewerLifecycleTests, RapidProfileChangesCoalesceToNewestRequest)
{
    auto secondBuildGate = std::make_shared<BuildGate>();
    auto buildCount = std::make_shared<std::atomic<int>> (0);
    createProcessor ([secondBuildGate, buildCount] (Request request)
    {
        const auto call = buildCount->fetch_add (1) + 1;
        if (call == 2)
            secondBuildGate->enterAndWait();
        return buildRuntime (std::move (request));
    });

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));

    processor->setAnalysisProfile (SpectrumAnalysisProfile::balanced);
    const auto buildEntered = secondBuildGate->waitUntilEntered();
    if (! buildEntered)
        secondBuildGate->release();
    ASSERT_TRUE (buildEntered);
    processor->setAnalysisProfile (SpectrumAnalysisProfile::fine);
    secondBuildGate->release();

    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::warmingUp
               && processor->getWarmupTargetSampleCount() == 160u;
    }));
    EXPECT_EQ (buildCount->load(), 3);

    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    ASSERT_TRUE (waitUntil ([this]
    {
        bool matched = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            matched = frame.descriptor.windowSampleCount == 160u;
            if (matched)
            {
                EXPECT_DOUBLE_EQ (frame.descriptor.timeHalfBandwidth, 3.0);
                EXPECT_EQ (frame.descriptor.taperCount, 4u);
            }
        });
        return matched;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, StreamChangePreparesThenAtomicallyReplacesRoute)
{
    auto replacementGate = std::make_shared<BuildGate>();
    auto buildCount = std::make_shared<std::atomic<int>> (0);
    auto requestedStream = std::make_shared<std::atomic<std::uint16_t>> (0);
    auto requestedGlobalChannel = std::make_shared<std::atomic<int>> (-1);
    createProcessor (
        [replacementGate, buildCount, requestedStream, requestedGlobalChannel] (Request request)
        {
            const auto call = buildCount->fetch_add (1) + 1;
            requestedStream->store (request.sourceStreamId);
            requestedGlobalChannel->store (
                request.sourceGlobalChannelIndices.empty()
                    ? -1
                    : request.sourceGlobalChannelIndices.front());
            if (call == 2)
                replacementGate->enterAndWait();
            return buildRuntime (std::move (request));
        },
        1,
        2);

    ASSERT_EQ (processor->getDataStreams().size(), 2);
    const auto firstStream = processor->getDataStreams()[0]->getStreamId();
    const auto secondStream = processor->getDataStreams()[1]->getStreamId();
    ASSERT_NE (firstStream, secondStream);

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    AudioBuffer<float> buffer (2, blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        buffer.setSample (0, sample, 1.0f);
        buffer.setSample (1, sample, sample % 2 == 0 ? 1.0f : -1.0f);
    }
    for (int block = 0; block < 4; ++block)
        tester->processBlock (processor, buffer);
    ASSERT_TRUE (waitUntil ([this, firstStream]
    {
        return consumeStreamId() == firstStream;
    }));

    auto* streamParameter = dynamic_cast<SelectedStreamParameter*> (
        processor->getParameter ("active_stream"));
    ASSERT_NE (streamParameter, nullptr);
    EXPECT_FALSE (streamParameter->shouldDeactivateDuringAcquisition());
    streamParameter->setNextValue (1, false);

    const auto replacementEntered = replacementGate->waitUntilEntered();
    if (! replacementEntered)
        replacementGate->release();
    ASSERT_TRUE (replacementEntered);
    EXPECT_TRUE (processor->hasActiveAnalysis());
    EXPECT_TRUE (processor->isAnalysisConfigurationPending());
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::live);
    EXPECT_EQ (requestedStream->load(), secondStream);
    EXPECT_EQ (requestedGlobalChannel->load(), 1);

    replacementGate->release();
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::warmingUp;
    }));
    for (int block = 0; block < 4; ++block)
        tester->processBlock (processor, buffer);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    ASSERT_TRUE (waitUntil ([this, secondStream]
    {
        auto matched = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            if (frame.sourceStreamId != secondStream)
                return;
            matched = true;
            EXPECT_GT (*std::max_element (frame.getChannelData (0),
                                          frame.getChannelData (0) + frame.numBins),
                       0.01f);
        });
        return matched;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, FailedStreamReplacementKeepsPreviousRouteLive)
{
    auto buildCount = std::make_shared<std::atomic<int>> (0);
    createProcessor (
        [buildCount] (Request request)
        {
            if (buildCount->fetch_add (1) == 1)
                throw std::runtime_error ("deliberate stream replacement failure");
            return buildRuntime (std::move (request));
        },
        1,
        2);

    const auto firstStream = processor->getDataStreams()[0]->getStreamId();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    auto* streamParameter = dynamic_cast<SelectedStreamParameter*> (
        processor->getParameter ("active_stream"));
    ASSERT_NE (streamParameter, nullptr);
    streamParameter->setNextValue (1, false);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness()
               == SpectrumAnalysisReadiness::configurationFailed;
    }));
    EXPECT_TRUE (processor->hasActiveAnalysis());

    AudioBuffer<float> buffer (2, blockSize);
    for (int sample = 0; sample < blockSize; ++sample)
    {
        buffer.setSample (0, sample, static_cast<float> (sample + 1));
        buffer.setSample (1, sample, static_cast<float> (20 + sample));
    }
    for (int block = 0; block < 4; ++block)
        tester->processBlock (processor, buffer);
    ASSERT_TRUE (waitUntil ([this, firstStream]
    {
        return consumeStreamId() == firstStream;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, FailedReplacementKeepsCurrentRuntimeLive)
{
    createProcessor ([] (Request request)
    {
        if (request.parameters.windowSampleCount == 40u)
            throw std::runtime_error ("deliberate replacement failure");
        return buildRuntime (std::move (request));
    });

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    EXPECT_EQ (consumeWindowSize(), 20u);

    processor->setAnalysisProfile (SpectrumAnalysisProfile::balanced);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::configurationFailed;
    }));
    EXPECT_TRUE (processor->hasActiveAnalysis());
    EXPECT_EQ (processor->getConfigurationFailureCount(), 1u);

    writeBlocks (2);
    ASSERT_TRUE (waitUntil ([this] { return consumeWindowSize() == 20u; }));
}

TEST_F (SpectrumViewerLifecycleTests, StopAndRestartDuringPreparationRejectsStaleRuntime)
{
    auto firstBuildGate = std::make_shared<BuildGate>();
    auto buildCount = std::make_shared<std::atomic<int>> (0);
    createProcessor ([firstBuildGate, buildCount] (Request request)
    {
        const auto call = buildCount->fetch_add (1) + 1;
        if (call == 1)
            firstBuildGate->enterAndWait();
        return buildRuntime (std::move (request));
    });

    ASSERT_TRUE (processor->startAcquisition());
    const auto buildEntered = firstBuildGate->waitUntilEntered();
    if (! buildEntered)
        firstBuildGate->release();
    ASSERT_TRUE (buildEntered);

    const auto stopped = processor->stopAcquisition();
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::stopped);
    EXPECT_FALSE (processor->hasActiveAnalysis());

    const auto restarted = processor->startAcquisition();
    firstBuildGate->release();
    ASSERT_TRUE (stopped);
    ASSERT_TRUE (restarted);
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::preparing);

    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::warmingUp;
    }));
    EXPECT_EQ (buildCount->load(), 2);
    EXPECT_EQ (processor->getWarmupTargetSampleCount(), 20u);

    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, BacklogShedsObsoleteWindowsAndThenResumesCadence)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    // Pause only the analysis consumer so the fake source can deterministically
    // fill the real processor FIFO without introducing a production test hook.
    ASSERT_TRUE (processor->stopThread (1000));
    writeBlocks (6);
    ASSERT_TRUE (processor->startThread (juce::Thread::Priority::normal));

    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
    EXPECT_EQ (processor->getShedSpectrumWindowCount(), 1u);

    std::int64_t firstSample = -1;
    std::uint64_t sequence = 0;
    ASSERT_TRUE (waitUntil ([this, &firstSample, &sequence]
    {
        return processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            firstSample = frame.firstSample;
            sequence = frame.sequence;
        });
    }));
    EXPECT_EQ (firstSample, 10);
    EXPECT_EQ (sequence, 1u);

    writeBlocks (2);
    ASSERT_TRUE (waitUntil ([this, &firstSample, &sequence]
    {
        return processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            firstSample = frame.firstSample;
            sequence = frame.sequence;
        });
    }));
    EXPECT_EQ (firstSample, 20);
    EXPECT_EQ (sequence, 2u);
    EXPECT_EQ (processor->getShedSpectrumWindowCount(), 1u);
}

TEST_F (SpectrumViewerLifecycleTests, PublishesFullRangeReducedFramesWithNativeUnits)
{
    createProcessor();
    processor->setDisplayColumnCount (4);
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);

    bool inspected = false;
    ASSERT_TRUE (waitUntil ([this, &inspected]
    {
        return processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            inspected = true;
            EXPECT_TRUE (frame.reducedForDisplay);
            EXPECT_EQ (frame.numBins, 4u);
            EXPECT_STREQ (frame.getSourceChannelUnit (0), "uV");
            EXPECT_EQ (frame.frequencyScale, spectrumviewer::FrequencyScale::linear);
            EXPECT_DOUBLE_EQ (frame.minimumFrequencyHz, 0.0);
            EXPECT_DOUBLE_EQ (frame.maximumFrequencyHz, sampleRate * 0.5);
            EXPECT_GT (frame.frequenciesHz[0], 0.0f);
            EXPECT_LT (frame.frequenciesHz[0], frame.frequenciesHz[3]);
            EXPECT_LT (frame.frequenciesHz[3], sampleRate * 0.5f);
        });
    }));
    EXPECT_TRUE (inspected);

    processor->setFrequencyScale (spectrumviewer::FrequencyScale::logarithmic);
    writeBlocks (2);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool matched = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            matched = frame.frequencyScale == spectrumviewer::FrequencyScale::logarithmic;
            if (matched)
            {
                EXPECT_DOUBLE_EQ (frame.minimumFrequencyHz, 4.0);
                EXPECT_GT (frame.frequenciesHz[0], 0.0f);
            }
        });
        return matched;
    }));

    processor->setFrequencyRange ({ 10, 30 });
    writeBlocks (2);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool matched = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            matched = frame.minimumFrequencyHz == 10.0
                      && frame.maximumFrequencyHz == 30.0;
            if (matched)
            {
                EXPECT_GT (frame.frequenciesHz[0], 10.0f);
                EXPECT_LT (frame.frequenciesHz[frame.numBins - 1], 30.0f);
            }
        });
        return matched;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, CapturesNonOverlappingFineWindowsAndReturnsToLive)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));

    ASSERT_TRUE (processor->startSpectrumCapture (3.0));
    EXPECT_EQ (processor->getCaptureState(), SpectrumCaptureState::preparing);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    EXPECT_EQ (processor->getCaptureTargetWindowCount(), 2u);
    EXPECT_EQ (processor->getWarmupTargetSampleCount(), 160u);

    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureIncludedWindowCount() == 1u;
    }));
    ASSERT_TRUE (waitUntil ([this]
    {
        bool progress = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            progress = frame.capture.product
                       == spectrumviewer::SpectrumFrameProduct::captureProgress;
            if (progress)
            {
                EXPECT_EQ (frame.descriptor.windowSampleCount, 160u);
                EXPECT_EQ (frame.descriptor.hopSampleCount, 160u);
                EXPECT_DOUBLE_EQ (frame.descriptor.timeHalfBandwidth, 3.0);
                EXPECT_EQ (frame.descriptor.taperCount, 4u);
                EXPECT_EQ (frame.capture.includedWindowCount, 1u);
                EXPECT_EQ (frame.capture.targetWindowCount, 2u);
            }
        });
        return progress;
    }));

    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (waitUntil ([this]
    {
        bool complete = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            complete = frame.capture.product
                       == spectrumviewer::SpectrumFrameProduct::captureComplete;
            if (complete)
            {
                EXPECT_EQ (frame.capture.includedWindowCount, 2u);
                EXPECT_EQ (frame.capture.lastSampleExclusive - frame.firstSample, 320);
                EXPECT_FALSE (frame.capture.hasQualityWarning());
            }
        });
        return complete;
    }));
    EXPECT_DOUBLE_EQ (processor->getCaptureAnalyzedSeconds(), 4.0);

    processor->returnToLive();
    EXPECT_EQ (processor->getCaptureState(), SpectrumCaptureState::restoringLive);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live
               && processor->getWarmupTargetSampleCount() == 20u;
    }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, FailedCapturePreparationLeavesLiveRuntimeUsable)
{
    createProcessor ([] (Request request)
    {
        if (request.captureTargetWindowCount > 0)
            throw std::runtime_error ("deliberate capture failure");
        return buildRuntime (std::move (request));
    });
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));

    ASSERT_TRUE (processor->startSpectrumCapture (10.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::failed;
    }));
    EXPECT_TRUE (processor->hasActiveAnalysis());
    EXPECT_EQ (processor->getConfigurationFailureCount(), 1u);

    writeBlocks (2);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool live = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            live = frame.capture.product == spectrumviewer::SpectrumFrameProduct::live;
        });
        return live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, CancellingCapturePreparationRestoresNewestLiveRequest)
{
    auto captureGate = std::make_shared<BuildGate>();
    createProcessor ([captureGate] (Request request)
    {
        if (request.captureTargetWindowCount > 0)
            captureGate->enterAndWait();
        return buildRuntime (std::move (request));
    });
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    ASSERT_TRUE (processor->startSpectrumCapture (10.0));
    const auto entered = captureGate->waitUntilEntered();
    if (! entered)
        captureGate->release();
    ASSERT_TRUE (entered);
    processor->cancelSpectrumCapture();
    EXPECT_EQ (processor->getCaptureState(), SpectrumCaptureState::restoringLive);
    captureGate->release();

    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live
               && processor->getWarmupTargetSampleCount() == 20u;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, StopDuringCaptureResetsStateForRestart)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (10.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));

    ASSERT_TRUE (processor->stopAcquisition());
    EXPECT_EQ (processor->getCaptureState(), SpectrumCaptureState::live);
    EXPECT_EQ (processor->getCaptureIncludedWindowCount(), 0u);
    EXPECT_EQ (processor->getAnalysisReadiness(), SpectrumAnalysisReadiness::stopped);

    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live
               && processor->hasActiveAnalysis();
    }));
}

TEST_F (SpectrumViewerLifecycleTests, FrozenCaptureReReducesWithoutNewInput)
{
    createProcessor();
    processor->setDisplayColumnCount (8);
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->consumeLatestSpectrumFrame ([] (const auto&) {});
    }));

    processor->setDisplayColumnCount (4);
    processor->setFrequencyScale (spectrumviewer::FrequencyScale::logarithmic);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool updated = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            updated = frame.capture.product
                          == spectrumviewer::SpectrumFrameProduct::captureComplete
                      && frame.frequencyScale
                             == spectrumviewer::FrequencyScale::logarithmic
                      && frame.numBins == 4u;
        });
        return updated;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, CaptureReportsInputGapWithoutMixingWindowHistory)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));

    ASSERT_TRUE (processor->stopThread (1000));
    writeBlocks (10);
    EXPECT_GT (processor->getDroppedInputBlockCount(), 0u);
    ASSERT_TRUE (processor->startThread (juce::Thread::Priority::normal));
    std::this_thread::sleep_for (20ms);
    writeBlocks (1);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getInputDiscontinuityCount() > 0;
    }));
    for (int block = 0;
         block < 96
         && processor->getCaptureState() != SpectrumCaptureState::frozen;
         ++block)
        writeBlocks (1);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));

    ASSERT_TRUE (waitUntil ([this]
    {
        bool warned = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            warned = frame.capture.product
                         == spectrumviewer::SpectrumFrameProduct::captureComplete
                     && frame.capture.discontinuityCount > 0
                     && frame.capture.hasQualityWarning();
        });
        return warned;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, FailedSpectrumExtendsCaptureAndMarksResult)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));

    writeBlocksWithValue (32, std::numeric_limits<float>::quiet_NaN());
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureFailedWindowCount() == 1u;
    }));
    EXPECT_EQ (processor->getCaptureIncludedWindowCount(), 0u);

    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (waitUntil ([this]
    {
        bool warned = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            warned = frame.capture.product
                         == spectrumviewer::SpectrumFrameProduct::captureComplete
                     && frame.capture.failedWindowCount == 1u
                     && frame.capture.hasQualityWarning();
        });
        return warned;
    }));
    EXPECT_DOUBLE_EQ (processor->getCaptureAnalyzedSeconds(), 2.0);
    EXPECT_DOUBLE_EQ (processor->getCaptureWallSpanSeconds(), 4.0);
}

TEST_F (SpectrumViewerLifecycleTests, RetriesCompletedCaptureAfterDisplayQueuePressure)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (10.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));

    for (std::size_t window = 1; window <= 5; ++window)
    {
        writeBlocks (32);
        ASSERT_TRUE (waitUntil ([this, window]
        {
            return processor->getCaptureIncludedWindowCount() >= window;
        }));
    }
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (processor->consumeLatestSpectrumFrame ([] (const auto&) {}));
    ASSERT_TRUE (waitUntil ([this]
    {
        bool complete = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            complete = frame.capture.product
                       == spectrumviewer::SpectrumFrameProduct::captureComplete;
        });
        return complete;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, CompletedCaptureBecomesImmutableReferenceAndReportsCompatibility)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));

    ASSERT_TRUE (processor->setCurrentCaptureAsReference());
    processor->setSpectrumComparisonMode (
        spectrumviewer::SpectrumComparisonMode::overlay);
    ASSERT_TRUE (waitUntil ([this] { return processor->hasSpectrumReference(); }));
    EXPECT_GT (processor->getReferenceCapturedAtMilliseconds(), 0);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool matched = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            matched = frame.comparison.mode
                          == spectrumviewer::SpectrumComparisonMode::overlay
                      && frame.comparison.compatibility
                             == spectrumviewer::SpectrumReferenceCompatibility::compatible
                      && frame.getChannelComparisonData (0) != nullptr;
            if (matched)
                for (std::size_t bin = 0; bin < frame.numBins; ++bin)
                    EXPECT_FLOAT_EQ (frame.getChannelComparisonData (0)[bin],
                                     frame.getChannelData (0)[bin]);
        });
        return matched;
    }));

    processor->returnToLive();
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live;
    }));
    EXPECT_TRUE (processor->hasSpectrumReference());
    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool compatible = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            compatible = frame.comparison.compatibility
                             == spectrumviewer::SpectrumReferenceCompatibility::compatible
                         && frame.getChannelComparisonData (0) != nullptr;
        });
        return compatible;
    }));

    processor->clearSpectrumReference();
    ASSERT_TRUE (waitUntil ([this] { return ! processor->hasSpectrumReference(); }));
    EXPECT_EQ (processor->getReferenceCompatibility(),
               spectrumviewer::SpectrumReferenceCompatibility::noReference);
    EXPECT_EQ (processor->getReferenceCapturedAtMilliseconds(), 0);
    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        bool absolute = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            absolute = frame.comparison.mode
                           == spectrumviewer::SpectrumComparisonMode::absolute
                       && frame.getChannelComparisonData (0) == nullptr;
        });
        return absolute;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, DeselectingEveryChannelReleasesTheLiveRoute)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));

    auto* stream = processor->getDataStreams()[0];
    ASSERT_NE (stream, nullptr);
    auto* channelParameter = dynamic_cast<SelectedChannelsParameter*> (
        stream->getParameter ("Channels"));
    ASSERT_NE (channelParameter, nullptr);

    channelParameter->setNextValue (Array<var> {}, false);
    EXPECT_TRUE (processor->getActiveChans().isEmpty());

    // The route the display was built from is gone, so the runtime holding it
    // must go too. Leaving it live is what kept the canvas drawing channels
    // the user had just deselected.
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness()
               == SpectrumAnalysisReadiness::invalidSelection;
    }));
    ASSERT_TRUE (waitUntil ([this] { return ! processor->hasActiveAnalysis(); }));

    // Blocks arriving against the released route are not mapping errors.
    const auto rejectedBefore = processor->getRejectedInputBlockCount();
    writeBlocks (4);
    EXPECT_EQ (processor->getRejectedInputBlockCount(), rejectedBefore);
    EXPECT_GT (processor->getUnconfiguredInputBlockCount(), 0u);

    // No frames are published while nothing is selected.
    auto framesPublished = false;
    processor->consumeLatestSpectrumFrame ([&] (const auto&) { framesPublished = true; });
    EXPECT_FALSE (framesPublished);

    // Selecting again rebuilds the route and returns to live.
    channelParameter->setNextValue (Array<var> { 0 }, false);
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    writeBlocks (4);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getAnalysisReadiness() == SpectrumAnalysisReadiness::live;
    }));
}

TEST_F (SpectrumViewerLifecycleTests, ReleasedRouteDiscardsAFrozenCaptureButKeepsTheReference)
{
    createProcessor();
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    writeBlocks (32);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (processor->setCurrentCaptureAsReference());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasSpectrumReference(); }));

    auto* stream = processor->getDataStreams()[0];
    ASSERT_NE (stream, nullptr);
    auto* channelParameter = dynamic_cast<SelectedChannelsParameter*> (
        stream->getParameter ("Channels"));
    ASSERT_NE (channelParameter, nullptr);
    channelParameter->setNextValue (Array<var> {}, false);

    // The frozen result is republished from the runtime's accumulator, so it
    // cannot outlive the runtime.
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live;
    }));
    EXPECT_EQ (processor->getCaptureIncludedWindowCount(), 0u);

    // The reference is a standalone snapshot and does survive.
    EXPECT_TRUE (processor->hasSpectrumReference());
}

TEST_F (SpectrumViewerLifecycleTests, DeltaComparesAreaWeightedLinearPower)
{
    createProcessor();
    processor->setAnalysisProfile (SpectrumAnalysisProfile::fine);
    processor->setDisplayColumnCount (16);
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    writeBlocksScaled (32, 1.0f);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));
    ASSERT_TRUE (processor->setCurrentCaptureAsReference());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasSpectrumReference(); }));

    processor->setSpectrumComparisonMode (
        spectrumviewer::SpectrumComparisonMode::deltaDb);
    processor->returnToLive();
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::live;
    }));
    ASSERT_TRUE (processor->startSpectrumCapture (2.0));
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::capturing;
    }));
    writeBlocksScaled (32, 2.0f);
    ASSERT_TRUE (waitUntil ([this]
    {
        return processor->getCaptureState() == SpectrumCaptureState::frozen;
    }));

    ASSERT_TRUE (waitUntil ([this]
    {
        bool checked = false;
        processor->consumeLatestSpectrumFrame ([&] (const auto& frame)
        {
            if (frame.comparison.mode
                    != spectrumviewer::SpectrumComparisonMode::deltaDb
                || frame.getChannelComparisonData (0) == nullptr)
                return;
            for (std::size_t bin = 0; bin < frame.numBins; ++bin)
            {
                const auto delta = frame.getChannelComparisonData (0)[bin];
                if (std::isfinite (delta))
                {
                    EXPECT_NEAR (delta, 10.0 * std::log10 (4.0), 0.02);
                    checked = true;
                }
            }
        });
        return checked;
    }));
}
} // namespace
