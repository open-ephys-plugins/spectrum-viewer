/* Processor-level tests for asynchronous Spectrum Viewer configuration. */

#include "gtest/gtest.h"

#include "SpectrumViewer.h"
#include <TestFixtures.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
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
                                      std::move (request.sourceChannelUnits));
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

    void createProcessor (spectrumviewer::AsyncSpectrumAnalysis::Builder builder = {})
    {
        tester = std::make_unique<ProcessorTester> (
            TestSourceNodeBuilder (FakeSourceNodeParams { 1, sampleRate, 1.0f }));
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
        AudioBuffer<float> buffer (1, blockSize);
        for (int sample = 0; sample < blockSize; ++sample)
            buffer.setSample (0, sample, static_cast<float> (sample + 1));

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
    ASSERT_TRUE (waitUntil ([this] { return consumeWindowSize() == 160u; }));
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
                EXPECT_GT (frame.frequenciesHz[0], 0.0f);
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
} // namespace
