/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2019 Translational NeuroEngineering Laboratory

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "SpectrumViewer.h"

#include "BacklogSheddingPolicy.h"
#include "SpectrumViewerEditor.h"

#include <cmath>
#include <limits>

namespace
{
struct ProfileSettings
{
    double windowSeconds;
    double hopSeconds;
    double timeHalfBandwidth;
    std::size_t taperCount;
};

ProfileSettings getProfileSettings (SpectrumAnalysisProfile profile)
{
    switch (profile)
    {
        case SpectrumAnalysisProfile::balanced:
            return { 0.5, 0.25, 2.5, 4 };
        case SpectrumAnalysisProfile::fine:
            return { 2.0, 0.5, 3.0, 4 };
        case SpectrumAnalysisProfile::fast:
        default:
            return { 0.25, 0.125, 2.0, 3 };
    }
}
} // namespace

#define MS_FROM_START Time::highResolutionTicksToSeconds (Time::getHighResolutionTicks() - start) * 1000

SpectrumViewer::SpectrumViewer (
    spectrumviewer::AsyncSpectrumAnalysis::Builder configurationBuilder)
    : GenericProcessor ("Spectrum Viewer"),
      Thread ("FFT Thread"),
      displayType (POWER_SPECTRUM),
      asynchronousAnalysis (std::move (configurationBuilder))
{
    tfrParams.segLen = 1;
    tfrParams.freqStart = 0;
    tfrParams.freqEnd = 1000;
    tfrParams.stepLen = 0.125; // Fast profile: 50% overlap
    tfrParams.winLen = 0.25;
    tfrParams.interpRatio = 1;
    tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
    tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);
    tfrParams.Fs = 2000;
    tfrParams.alpha = 0;
    tfrParams.nTimes = 1;
}

SpectrumViewer::~SpectrumViewer()
{
    acquisitionRunning.store (false, std::memory_order_release);
    activeConfigurationGeneration.store (0, std::memory_order_release);
    activeBinWidthHz.store (0.0f, std::memory_order_release);
    stopThread (1000);
    if (activeAnalysis != nullptr)
        asynchronousAnalysis.retire (activeAnalysis);
    {
        const std::lock_guard<std::mutex> lock (displayAnalysisMutex);
        displayAnalysis.reset();
    }
    activeAnalysis.reset();
}

void SpectrumViewer::registerParameters()
{
    addSelectedStreamParameter (Parameter::PROCESSOR_SCOPE,
                                "active_stream",
                                "Display Stream",
                                "Currently selected stream",
                                {},
                                0,
                                true,
                                true);

    addSelectedChannelsParameter (Parameter::STREAM_SCOPE,
                                  "Channels",
                                  "Channels",
                                  "The channels to analyze",
                                  MAX_CHANS,
                                  false);
}

AudioProcessorEditor* SpectrumViewer::createEditor()
{
    editor = std::make_unique<SpectrumViewerEditor> (this);
    return editor.get();
}

void SpectrumViewer::parameterValueChanged (Parameter* param)
{
    if (param->getName().equalsIgnoreCase ("active_stream"))
    {
        String streamKey = param->getValueAsString();

        if (streamKey.isEmpty())
            return;

        LOGC ("Setting active stream to: ", streamKey);

        activeStream = getDataStream (streamKey)->getStreamId();

        tfrParams.Fs = getDataStream (activeStream)->getSampleRate();
        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        SelectedChannelsParameter* p = (SelectedChannelsParameter*) getDataStream (activeStream)->getParameter ("Channels");
        if (p != nullptr)
        {
            channels = p->getArrayValue();
            if (auto* currentEditor = getEditor())
                currentEditor->updateVisualizer();
        }
    }
    else if (param->getName() == "Channels")
    {
        channels.clear();

        SelectedChannelsParameter* p = (SelectedChannelsParameter*) param;

        channels = p->getArrayValue();

        if (auto* currentEditor = getEditor())
            currentEditor->updateVisualizer();
    }
}

void SpectrumViewer::setFrequencyRange (Range<int> newRange)
{
    beginDisplaySettingsUpdate();
    displayMinimumFrequencyHz.store (static_cast<double> (newRange.getStart()),
                                     std::memory_order_relaxed);
    displayMaximumFrequencyHz.store (static_cast<double> (newRange.getEnd()),
                                     std::memory_order_relaxed);
    endDisplaySettingsUpdate();
    if (newRange.getEnd() != tfrParams.freqEnd)
    {
        tfrParams.freqEnd = newRange.getEnd();

        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        if (auto* currentEditor = getEditor())
            currentEditor->updateVisualizer();
    }
}

SpectrumViewer::DisplaySettings SpectrumViewer::readDisplaySettings() const noexcept
{
    DisplaySettings settings;
    for (;;)
    {
        const auto before = displaySettingsSequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue;

        settings.columnCount = displayColumnCount.load (std::memory_order_relaxed);
        settings.frequencyScale = displayFrequencyScale.load (std::memory_order_relaxed);
        settings.minimumFrequencyHz = displayMinimumFrequencyHz.load (std::memory_order_relaxed);
        settings.maximumFrequencyHz = displayMaximumFrequencyHz.load (std::memory_order_relaxed);

        if (displaySettingsSequence.load (std::memory_order_acquire) == before)
        {
            settings.sequence = before;
            return settings;
        }
    }
}

void SpectrumViewer::applyReferenceRequest() noexcept
{
    const auto request = referenceRequest.exchange (0, std::memory_order_acq_rel);
    if (request == 0)
        return;

    if (request == CLEAR_REFERENCE_REQUEST)
    {
        spectrumReference.reset();
        referenceCapturedAtMilliseconds.store (0, std::memory_order_relaxed);
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::noReference,
            std::memory_order_relaxed);
        referenceCaptureId.store (0, std::memory_order_release);
    }
    else if (completedCapture != nullptr
             && completedCapture->getCaptureId() == request)
    {
        spectrumReference = completedCapture;
        referenceCapturedAtMilliseconds.store (
            completedCapture->getCapturedAtUnixMilliseconds(),
            std::memory_order_relaxed);
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::compatible,
            std::memory_order_relaxed);
        referenceCaptureId.store (request, std::memory_order_release);
    }

    captureCompletionPending = captureState.load (std::memory_order_relaxed)
                               == SpectrumCaptureState::frozen;
}

void SpectrumViewer::finalizeCapturedSpectrum()
{
    if (activeAnalysis == nullptr || ! activeAnalysis->isCaptureRuntime()
        || completedCapture != nullptr)
        return;

    auto* accumulator = activeAnalysis->getCaptureAccumulator();
    if (accumulator == nullptr || ! accumulator->isComplete())
        return;

    try
    {
        const auto valueCount = accumulator->getChannelCount()
                                * accumulator->getBinCount();
        std::vector<float> mean (accumulator->getPlanarMean(),
                                 accumulator->getPlanarMean() + valueCount);
        std::vector<float> variance (valueCount);
        for (std::size_t channel = 0; channel < accumulator->getChannelCount(); ++channel)
            for (std::size_t bin = 0; bin < accumulator->getBinCount(); ++bin)
                variance[channel * accumulator->getBinCount() + bin]
                    = accumulator->getSampleVariance (channel, bin);

        const auto& pipeline = activeAnalysis->getPipeline();
        spectrumviewer::SpectrumCaptureQuality quality;
        quality.includedWindowCount = accumulator->getIncludedWindowCount();
        quality.targetWindowCount = accumulator->getTargetWindowCount();
        quality.firstSample = captureFirstSample;
        quality.lastSampleExclusive = captureLastSampleExclusive;
        quality.failedWindowCount = pipeline.getFailedWindowCount()
                                    + accumulator->getRejectedWindowCount();
        quality.shedWindowCount = pipeline.getShedWindowCount();
        quality.discontinuityCount = pipeline.getDiscontinuityCount();

        const auto& frameFifo = activeAnalysis->getFrameFifo();
        completedCapture = std::make_shared<const spectrumviewer::CapturedSpectrum> (
            activeAnalysis->getCaptureId(),
            Time::currentTimeMillis(),
            activeAnalysis->getConfiguration().getFrameDescriptor(),
            frameFifo.getSourceChannelIndices(),
            frameFifo.getSourceChannelUnits(),
            std::move (mean),
            std::move (variance),
            quality);
    }
    catch (const std::exception& error)
    {
        LOGE ("Unable to retain completed spectrum capture: ", error.what());
    }
}

bool SpectrumViewer::publishReducedSpectrum (
    const float* planarPsd,
    std::size_t channelCount,
    std::size_t binCount,
    const spectrumviewer::SpectrumFrameDescriptor& descriptor,
    std::int64_t firstSample,
    std::uint64_t sequence,
    spectrumviewer::SpectrumCaptureFrameStatus capture) noexcept
{
    if (activeAnalysis == nullptr)
        return false;
    auto& frameFifo = activeAnalysis->getFrameFifo();
    if (frameFifo.getNumReady() >= frameFifo.getCapacity())
        return false;

    const auto settings = readDisplaySettings();
    auto maximumHz = settings.maximumFrequencyHz;
    if (maximumHz <= 0.0)
        maximumHz = descriptor.sampleRateHz * 0.5;

    auto& reducer = activeAnalysis->getDisplayReducer();
    if (! reducer.reduce (planarPsd,
                          channelCount,
                          binCount,
                          descriptor.sampleRateHz,
                          descriptor.windowSampleCount,
                          settings.columnCount,
                          settings.frequencyScale,
                          settings.minimumFrequencyHz,
                          maximumHz))
        return false;

    spectrumviewer::SpectrumComparisonFrameStatus comparison;
    const float* comparisonData = nullptr;
    const auto requestedMode = comparisonMode.load (std::memory_order_acquire);
    if (spectrumReference == nullptr)
    {
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::noReference,
            std::memory_order_release);
    }
    else
    {
        const auto compatible = spectrumReference->isCompatibleWith (
            descriptor,
            frameFifo.getSourceChannelIndices(),
            frameFifo.getSourceChannelUnits());
        referenceCompatibility.store (
            compatible ? spectrumviewer::SpectrumReferenceCompatibility::compatible
                       : spectrumviewer::SpectrumReferenceCompatibility::incompatible,
            std::memory_order_release);

        if (requestedMode != spectrumviewer::SpectrumComparisonMode::absolute)
        {
            comparison.mode = requestedMode;
            comparison.referenceCaptureId = spectrumReference->getCaptureId();
            comparison.compatibility = compatible
                                           ? spectrumviewer::SpectrumReferenceCompatibility::compatible
                                           : spectrumviewer::SpectrumReferenceCompatibility::incompatible;
            if (compatible)
            {
                auto& referenceReducer = activeAnalysis->getReferenceDisplayReducer();
                if (! referenceReducer.reduce (
                        spectrumReference->getPlanarMeanPsd(),
                        spectrumReference->getChannelCount(),
                        spectrumReference->getBinCount(),
                        descriptor.sampleRateHz,
                        descriptor.windowSampleCount,
                        settings.columnCount,
                        settings.frequencyScale,
                        settings.minimumFrequencyHz,
                        maximumHz))
                    return false;

                const auto& current = reducer.getView();
                const auto& reference = referenceReducer.getView();
                if (requestedMode == spectrumviewer::SpectrumComparisonMode::overlay)
                    comparisonData = reference.getChannelMean (0);
                else
                {
                    auto* delta = activeAnalysis->getComparisonScratch();
                    const auto count = current.numChannels * current.numColumns;
                    spectrumviewer::computeDecibelDelta (
                        current.getChannelMean (0),
                        reference.getChannelMean (0),
                        delta,
                        count);
                    comparisonData = delta;
                }
            }
        }
    }

    const auto& reduced = reducer.getView();
    return frameFifo.tryPushReduced (reduced.getChannelMean (0),
                                     reduced.getChannelPeak (0),
                                     reduced.frequenciesHz,
                                     reduced.numChannels,
                                     reduced.numColumns,
                                     firstSample,
                                     sequence,
                                     reduced.frequencyScale,
                                     reduced.minimumFrequencyHz,
                                     reduced.maximumFrequencyHz,
                                     capture,
                                     comparisonData,
                                     comparison);
}

bool SpectrumViewer::publishCapturedSpectrum (bool complete) noexcept
{
    if (activeAnalysis == nullptr || ! activeAnalysis->isCaptureRuntime())
        return false;
    auto* accumulator = activeAnalysis->getCaptureAccumulator();
    if (accumulator == nullptr || accumulator->getIncludedWindowCount() == 0
        || ! captureHasFirstSample)
        return false;
    const auto& descriptor = activeAnalysis->getConfiguration().getFrameDescriptor();

    spectrumviewer::SpectrumCaptureFrameStatus status;
    status.product = complete ? spectrumviewer::SpectrumFrameProduct::captureComplete
                              : spectrumviewer::SpectrumFrameProduct::captureProgress;
    status.captureId = activeAnalysis->getCaptureId();
    status.includedWindowCount = accumulator->getIncludedWindowCount();
    status.targetWindowCount = accumulator->getTargetWindowCount();
    status.lastSampleExclusive = captureLastSampleExclusive;
    const auto& pipeline = activeAnalysis->getPipeline();
    status.failedWindowCount = pipeline.getFailedWindowCount()
                               + accumulator->getRejectedWindowCount();
    status.shedWindowCount = pipeline.getShedWindowCount();
    status.discontinuityCount = pipeline.getDiscontinuityCount();

    const auto published = publishReducedSpectrum (
        accumulator->getPlanarMean(),
        accumulator->getChannelCount(),
        accumulator->getBinCount(),
        descriptor,
        captureFirstSample,
        captureLastFrameSequence,
        status);
    if (published)
    {
        const auto settings = readDisplaySettings();
        captureLastPublishedDisplaySettings = settings.sequence;
        captureLastPublishedComparisonSettings = comparisonSettingsSequence.load (
            std::memory_order_acquire);
    }
    return published;
}

void SpectrumViewer::process (AudioBuffer<float>& continuousBuffer)
{
    auto* fifo = inputFifo.get();
    if (fifo == nullptr || acquisitionChannelCount == 0
        || ! acquisitionRunning.load (std::memory_order_acquire))
        return;

    const auto incomingSampleCount = static_cast<std::size_t> (getNumSamplesInBlock (activeStream));
    if (incomingSampleCount == 0)
        return;

    const auto generation = activeConfigurationGeneration.load (std::memory_order_acquire);
    if (generation == 0)
    {
        unconfiguredInputBlocks.fetch_add (1, std::memory_order_relaxed);
        unconfiguredInputSamples.fetch_add (incomingSampleCount, std::memory_order_relaxed);
        return;
    }

    std::array<const float*, MAX_CHANS> channelData {};

    for (std::size_t channel = 0; channel < acquisitionChannelCount; ++channel)
    {
        const auto globalChannel = getGlobalChannelIndex (activeStream, acquisitionChannels[channel]);
        if (globalChannel < 0)
            return;

        channelData[channel] = continuousBuffer.getReadPointer (globalChannel);
    }

    if (! fifo->tryPush (channelData.data(),
                         acquisitionChannelCount,
                         incomingSampleCount,
                         getFirstSampleNumberForBlock (activeStream),
                         generation))
    {
        droppedInputBlocks.store (fifo->getDroppedBlockCount(), std::memory_order_relaxed);
        droppedInputSamples.store (fifo->getDroppedSampleCount(), std::memory_order_relaxed);
        rejectedInputBlocks.store (fifo->getRejectedBlockCount(), std::memory_order_relaxed);
    }
}

void SpectrumViewer::run()
{
    while (! threadShouldExit())
    {
        adoptPreparedAnalysis();
        applyReferenceRequest();
        if (activeAnalysis != nullptr && activeAnalysis->isCaptureRuntime()
            && captureState.load (std::memory_order_acquire)
                   == SpectrumCaptureState::frozen)
        {
            const auto displayVersion = displaySettingsSequence.load (std::memory_order_acquire);
            const auto comparisonVersion = comparisonSettingsSequence.load (
                std::memory_order_acquire);
            const auto& frameFifo = activeAnalysis->getFrameFifo();
            if ((displayVersion & 1u) == 0u
                && (captureCompletionPending
                    || displayVersion != captureLastPublishedDisplaySettings
                    || comparisonVersion != captureLastPublishedComparisonSettings)
                && frameFifo.getNumReady() < frameFifo.getCapacity())
                captureCompletionPending = ! publishCapturedSpectrum (true);
        }
        bool consumedBlock = false;
        auto* fifo = inputFifo.get();
        auto* pipeline = activeAnalysis != nullptr ? &activeAnalysis->getPipeline() : nullptr;
        spectrumviewer::BacklogSheddingPolicy sheddingPolicy;

        if (fifo != nullptr && pipeline != nullptr)
        {
            while (! threadShouldExit())
            {
                adoptPreparedAnalysis();
                pipeline = activeAnalysis != nullptr ? &activeAnalysis->getPipeline() : nullptr;
                if (pipeline == nullptr)
                    break;

                if (activeAnalysis->isCaptureRuntime()
                    && captureState.load (std::memory_order_acquire)
                           != SpectrumCaptureState::capturing)
                {
                    const auto popped = fifo->tryPop ([] (const auto&) {});
                    if (! popped)
                        break;
                    consumedBlock = true;
                    continue;
                }

                spectrumviewer::SpectrumAnalysisPipeline::AppendResult appendResult;
                const auto appendBlock = [&] (const auto& block)
                {
                    std::array<const float*, MAX_CHANS> channelData {};
                    for (std::size_t channel = 0; channel < block.numChannels; ++channel)
                        channelData[channel] = block.getChannelData (channel);

                    // appendBlock copies the complete block. Returning from this
                    // callback releases the FIFO slot before any FFT work begins.
                    appendResult = pipeline->appendBlock (channelData.data(),
                                                          block.numChannels,
                                                          block.numSamples,
                                                          block.firstSample,
                                                          block.configurationGeneration);
                    if (appendResult.status
                            == spectrumviewer::SpectrumAnalysisPipeline::AppendStatus::accepted
                        && activeAnalysis->isCaptureRuntime()
                        && ! captureHasFirstSample)
                    {
                        captureFirstSample = block.firstSample;
                        captureHasFirstSample = true;
                    }
                };
                const auto popped = fifo->tryPop (appendBlock);

                if (! popped)
                    break;

                consumedBlock = true;
                const auto sheddingAction = sheddingPolicy.inputBlockDequeued (
                    fifo->getNumReady() > 0);
                if (appendResult.status != spectrumviewer::SpectrumAnalysisPipeline::AppendStatus::accepted)
                {
                    if (appendResult.status == spectrumviewer::SpectrumAnalysisPipeline::AppendStatus::configurationMismatch)
                    {
                        staleConfigurationBlocks.fetch_add (1, std::memory_order_relaxed);
                        continue;
                    }

                    LOGE ("Spectrum Viewer worker rejected an invalid FIFO block");
                    continue;
                }

                if (appendResult.discontinuity)
                    inputDiscontinuities.store (pipeline->getDiscontinuityCount(), std::memory_order_relaxed);

                if (sheddingAction
                    != spectrumviewer::BacklogSheddingPolicy::Action::processAll)
                {
                    const auto shed = sheddingAction
                                              == spectrumviewer::BacklogSheddingPolicy::Action::discardAll
                                          ? pipeline->discardReadyFrames()
                                          : pipeline->discardReadyFramesExceptLatest();
                    shedSpectrumWindows.fetch_add (shed, std::memory_order_relaxed);
                    if (sheddingAction
                        == spectrumviewer::BacklogSheddingPolicy::Action::discardAll)
                        continue;
                }

                const auto publishFrame = [this] (const auto& frame)
                {
                    if (activeAnalysis->isCaptureRuntime())
                    {
                        auto* accumulator = activeAnalysis->getCaptureAccumulator();
                        if (accumulator == nullptr
                            || ! accumulator->add (frame.getChannelData (0),
                                                   frame.numChannels,
                                                   frame.numBins))
                            return;
                        captureLastSampleExclusive = frame.firstSample
                                                     + static_cast<std::int64_t> (
                                                         frame.descriptor.windowSampleCount);
                        captureLastFrameSequence = frame.sequence;
                        captureIncludedWindows.store (
                            accumulator->getIncludedWindowCount(),
                            std::memory_order_release);
                        captureWallSpanSeconds.store (
                            static_cast<double> (captureLastSampleExclusive
                                                 - captureFirstSample)
                                / frame.descriptor.sampleRateHz,
                            std::memory_order_relaxed);
                        const auto complete = accumulator->isComplete();
                        if (complete)
                            finalizeCapturedSpectrum();
                        const auto published = publishCapturedSpectrum (complete);
                        if (complete)
                        {
                            captureCompletionPending = ! published;
                            captureState.store (SpectrumCaptureState::frozen,
                                                std::memory_order_release);
                        }
                        return;
                    }

                    publishReducedSpectrum (frame.getChannelData (0),
                                            frame.numChannels,
                                            frame.numBins,
                                            frame.descriptor,
                                            frame.firstSample,
                                            frame.sequence);
                };
                auto maximumFrames = std::numeric_limits<std::size_t>::max();
                if (activeAnalysis->isCaptureRuntime())
                {
                    const auto* accumulator = activeAnalysis->getCaptureAccumulator();
                    maximumFrames = accumulator->getTargetWindowCount()
                                    - accumulator->getIncludedWindowCount();
                }
                pipeline->consumeReadyFrames (publishFrame, maximumFrames);
                failedSpectrumWindows.store (pipeline->getFailedWindowCount(), std::memory_order_relaxed);
                if (activeAnalysis->isCaptureRuntime())
                {
                    captureFailedWindows.store (
                        pipeline->getFailedWindowCount()
                            + activeAnalysis->getCaptureAccumulator()->getRejectedWindowCount(),
                        std::memory_order_relaxed);
                    captureShedWindows.store (pipeline->getShedWindowCount(),
                                              std::memory_order_relaxed);
                    captureDiscontinuities.store (pipeline->getDiscontinuityCount(),
                                                  std::memory_order_relaxed);
                }

                if (analysisReadiness.load (std::memory_order_relaxed)
                    == SpectrumAnalysisReadiness::warmingUp)
                {
                    warmupSampleCount.store (pipeline->getBufferedSampleCount(), std::memory_order_relaxed);
                    if (appendResult.windowsReady > 0)
                        analysisReadiness.store (SpectrumAnalysisReadiness::live, std::memory_order_release);
                }
            }
        }

        // Thread::notify() takes a mutex in JUCE 8, so the audio callback must not
        // use it. A short worker-only timed wait avoids both callback locks and spin.
        if (! consumedBlock)
            wait (2.0);
    }
}

void SpectrumViewer::adoptPreparedAnalysis()
{
    spectrumviewer::SpectrumAnalysisPreparationResult result;
    if (! asynchronousAnalysis.tryTakeLatest (result))
        return;

    const auto requested = requestedConfigurationGeneration.load (std::memory_order_acquire);
    if (result.generation != requested || ! acquisitionRunning.load (std::memory_order_acquire))
    {
        if (result.analysis != nullptr)
            asynchronousAnalysis.retire (std::move (result.analysis));
        return;
    }

    configurationPending.store (false, std::memory_order_release);
    if (! result.succeeded())
    {
        configurationFailures.fetch_add (1, std::memory_order_relaxed);
        if (result.captureId != 0)
        {
            captureState.store (
                activeAnalysis != nullptr && activeAnalysis->isCaptureRuntime()
                    ? SpectrumCaptureState::frozen
                    : SpectrumCaptureState::failed,
                std::memory_order_release);
        }
        else if (captureState.load (std::memory_order_relaxed)
                 == SpectrumCaptureState::restoringLive)
            captureState.store (SpectrumCaptureState::frozen,
                                std::memory_order_release);
        if (activeAnalysis == nullptr)
            activeConfigurationGeneration.store (0, std::memory_order_release);
        analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                 std::memory_order_release);
        LOGE ("Unable to prepare Spectrum Viewer analysis: ", result.error);
        return;
    }

    if (activeAnalysis != nullptr)
        asynchronousAnalysis.retire (activeAnalysis);

    activeAnalysis = std::move (result.analysis);
    {
        const std::lock_guard<std::mutex> lock (displayAnalysisMutex);
        displayAnalysis = activeAnalysis;
    }
    warmupSampleCount.store (0, std::memory_order_relaxed);
    warmupTargetSampleCount.store (
        activeAnalysis->getConfiguration().getParameters().windowSampleCount,
        std::memory_order_relaxed);
    analysisReadiness.store (SpectrumAnalysisReadiness::warmingUp, std::memory_order_release);
    activeBinWidthHz.store (
        static_cast<float> (activeAnalysis->getConfiguration().getFrameDescriptor().binWidthHz),
        std::memory_order_release);

    captureHasFirstSample = false;
    captureCompletionPending = false;
    captureFirstSample = 0;
    captureLastSampleExclusive = 0;
    captureLastFrameSequence = 0;
    captureLastPublishedDisplaySettings = displaySettingsSequence.load (std::memory_order_acquire);
    captureLastPublishedComparisonSettings = comparisonSettingsSequence.load (
        std::memory_order_acquire);
    captureIncludedWindows.store (0, std::memory_order_release);
    captureWallSpanSeconds.store (0.0, std::memory_order_relaxed);
    captureFailedWindows.store (0, std::memory_order_relaxed);
    captureShedWindows.store (0, std::memory_order_relaxed);
    captureDiscontinuities.store (0, std::memory_order_relaxed);
    if (activeAnalysis->isCaptureRuntime())
    {
        completedCapture.reset();
        const auto& descriptor = activeAnalysis->getConfiguration().getFrameDescriptor();
        captureWindowSeconds.store (
            static_cast<double> (descriptor.windowSampleCount)
                / descriptor.sampleRateHz,
            std::memory_order_relaxed);
        requestedCaptureId.store (activeAnalysis->getCaptureId(), std::memory_order_release);
        captureTargetWindows.store (
            activeAnalysis->getCaptureAccumulator()->getTargetWindowCount(),
            std::memory_order_release);
        captureState.store (SpectrumCaptureState::capturing, std::memory_order_release);
    }
    else
    {
        captureTargetWindows.store (0, std::memory_order_release);
        captureState.store (SpectrumCaptureState::live, std::memory_order_release);
    }

    // Publication of the generation is the callback's permission to enqueue
    // blocks for this runtime. Older queued blocks are rejected at the worker
    // boundary instead of being mixed into the new history.
    activeConfigurationGeneration.store (result.generation, std::memory_order_release);
}

void SpectrumViewer::updateSettings()
{
    if (dataStreams.size() > 0)
    {
        parameterValueChanged (getDataStream (activeStream)->getParameter ("Channels"));
    }
    else
    {
        channels.clear();
    }
}

Array<int> SpectrumViewer::getActiveChans()
{
    return channels;
}

const String SpectrumViewer::getChanName (int localIdx)
{
    return getDataStream (activeStream)->getContinuousChannels()[localIdx]->getName();
};

bool SpectrumViewer::startAcquisition()
{
    if (isEnabled)
    {
        acquisitionChannelCount = static_cast<std::size_t> (std::min (channels.size(), MAX_CHANS));
        const auto maximumInputBlockSamples = getBlockSize();
        const auto sampleRate = static_cast<double> (tfrParams.Fs);
        if (acquisitionChannelCount == 0 || maximumInputBlockSamples <= 0
            || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
        {
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
            return false;
        }

        for (std::size_t channel = 0; channel < acquisitionChannelCount; ++channel)
        {
            acquisitionChannels[channel] = channels[static_cast<int> (channel)];
            acquisitionChannelUnits[channel] = getDataStream (activeStream)
                                                   ->getContinuousChannels()[acquisitionChannels[channel]]
                                                   ->getUnits()
                                                   .toStdString();
        }

        try
        {
            inputFifo = std::make_unique<spectrumviewer::SampleBlockFifo> (
                acquisitionChannelCount,
                static_cast<std::size_t> (maximumInputBlockSamples),
                INPUT_QUEUE_CAPACITY);
        }
        catch (const std::exception& error)
        {
            LOGE ("Unable to configure Spectrum Viewer input: ", error.what());
            inputFifo.reset();
            acquisitionChannelCount = 0;
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
            return false;
        }

        acquisitionMaximumInputBlockSamples = static_cast<std::size_t> (maximumInputBlockSamples);
        acquisitionSampleRateHz = sampleRate;
        activeConfigurationGeneration.store (0, std::memory_order_release);
        activeBinWidthHz.store (0.0f, std::memory_order_release);
        droppedInputBlocks.store (0, std::memory_order_relaxed);
        droppedInputSamples.store (0, std::memory_order_relaxed);
        rejectedInputBlocks.store (0, std::memory_order_relaxed);
        inputDiscontinuities.store (0, std::memory_order_relaxed);
        failedSpectrumWindows.store (0, std::memory_order_relaxed);
        shedSpectrumWindows.store (0, std::memory_order_relaxed);
        unconfiguredInputBlocks.store (0, std::memory_order_relaxed);
        unconfiguredInputSamples.store (0, std::memory_order_relaxed);
        staleConfigurationBlocks.store (0, std::memory_order_relaxed);
        configurationFailures.store (0, std::memory_order_relaxed);
        requestedCaptureId.store (0, std::memory_order_relaxed);
        captureWindowSeconds.store (0.0, std::memory_order_relaxed);
        captureTargetWindows.store (0, std::memory_order_relaxed);
        captureIncludedWindows.store (0, std::memory_order_relaxed);
        captureWallSpanSeconds.store (0.0, std::memory_order_relaxed);
        captureFailedWindows.store (0, std::memory_order_relaxed);
        captureShedWindows.store (0, std::memory_order_relaxed);
        captureDiscontinuities.store (0, std::memory_order_relaxed);
        captureState.store (SpectrumCaptureState::live, std::memory_order_release);
        referenceRequest.store (0, std::memory_order_relaxed);
        referenceCaptureId.store (0, std::memory_order_relaxed);
        referenceCapturedAtMilliseconds.store (0, std::memory_order_relaxed);
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::noReference,
            std::memory_order_relaxed);
        completedCapture.reset();
        spectrumReference.reset();
        warmupSampleCount.store (0, std::memory_order_relaxed);
        warmupTargetSampleCount.store (0, std::memory_order_relaxed);
        analysisReadiness.store (SpectrumAnalysisReadiness::preparing, std::memory_order_release);
        acquisitionRunning.store (true, std::memory_order_release);
        startThread (Thread::Priority::normal);
        requestAnalysisConfiguration();
    }
    return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
    acquisitionRunning.store (false, std::memory_order_release);
    activeConfigurationGeneration.store (0, std::memory_order_release);
    activeBinWidthHz.store (0.0f, std::memory_order_release);
    stopThread (1000);
    if (activeAnalysis != nullptr)
        asynchronousAnalysis.retire (activeAnalysis);
    {
        const std::lock_guard<std::mutex> lock (displayAnalysisMutex);
        displayAnalysis.reset();
    }
    activeAnalysis.reset();
    inputFifo.reset();
    acquisitionChannelCount = 0;
    acquisitionMaximumInputBlockSamples = 0;
    acquisitionSampleRateHz = 0.0;
    configurationPending.store (false, std::memory_order_release);
    captureTargetWindows.store (0, std::memory_order_relaxed);
    captureIncludedWindows.store (0, std::memory_order_relaxed);
    captureWindowSeconds.store (0.0, std::memory_order_relaxed);
    captureWallSpanSeconds.store (0.0, std::memory_order_relaxed);
    captureFailedWindows.store (0, std::memory_order_relaxed);
    captureShedWindows.store (0, std::memory_order_relaxed);
    captureDiscontinuities.store (0, std::memory_order_relaxed);
    captureState.store (SpectrumCaptureState::live, std::memory_order_release);
    referenceRequest.store (0, std::memory_order_relaxed);
    referenceCaptureId.store (0, std::memory_order_relaxed);
    referenceCapturedAtMilliseconds.store (0, std::memory_order_relaxed);
    referenceCompatibility.store (
        spectrumviewer::SpectrumReferenceCompatibility::noReference,
        std::memory_order_relaxed);
    completedCapture.reset();
    spectrumReference.reset();
    analysisReadiness.store (SpectrumAnalysisReadiness::stopped, std::memory_order_release);
    return true;
}

void SpectrumViewer::setAnalysisProfile (SpectrumAnalysisProfile profile)
{
    if (profile != SpectrumAnalysisProfile::fast
        && profile != SpectrumAnalysisProfile::balanced
        && profile != SpectrumAnalysisProfile::fine)
        return;

    analysisProfile.store (profile, std::memory_order_relaxed);
    const auto settings = getProfileSettings (profile);
    tfrParams.winLen = static_cast<float> (settings.windowSeconds);
    tfrParams.stepLen = static_cast<float> (settings.hopSeconds);
    tfrParams.freqStep = 1.0f / tfrParams.winLen;
    tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

    if (acquisitionRunning.load (std::memory_order_acquire)
        && (captureState.load (std::memory_order_acquire) == SpectrumCaptureState::live
            || captureState.load (std::memory_order_acquire) == SpectrumCaptureState::failed))
        requestAnalysisConfiguration();
}

bool SpectrumViewer::startSpectrumCapture (double durationSeconds)
{
    const auto captureWindowSeconds = getProfileSettings (
        SpectrumAnalysisProfile::fine).windowSeconds;
    if (! acquisitionRunning.load (std::memory_order_acquire)
        || ! std::isfinite (durationSeconds) || durationSeconds <= 0.0)
        return false;

    const auto target = std::ceil (durationSeconds / captureWindowSeconds);
    if (target < 1.0
        || target > static_cast<double> (std::numeric_limits<std::size_t>::max()))
        return false;

    requestedCaptureId.store (nextCaptureId++, std::memory_order_release);
    captureTargetWindows.store (static_cast<std::size_t> (target),
                                std::memory_order_release);
    captureIncludedWindows.store (0, std::memory_order_release);
    captureWallSpanSeconds.store (0.0, std::memory_order_relaxed);
    captureFailedWindows.store (0, std::memory_order_relaxed);
    captureShedWindows.store (0, std::memory_order_relaxed);
    captureDiscontinuities.store (0, std::memory_order_relaxed);
    captureState.store (SpectrumCaptureState::preparing, std::memory_order_release);
    requestAnalysisConfiguration (true);
    return true;
}

bool SpectrumViewer::setCurrentCaptureAsReference() noexcept
{
    if (captureState.load (std::memory_order_acquire) != SpectrumCaptureState::frozen)
        return false;
    const auto captureId = requestedCaptureId.load (std::memory_order_acquire);
    if (captureId == 0)
        return false;
    referenceRequest.store (captureId, std::memory_order_release);
    comparisonSettingsSequence.fetch_add (1, std::memory_order_release);
    return true;
}

void SpectrumViewer::clearSpectrumReference() noexcept
{
    referenceRequest.store (CLEAR_REFERENCE_REQUEST, std::memory_order_release);
    comparisonSettingsSequence.fetch_add (1, std::memory_order_release);
}

void SpectrumViewer::setSpectrumComparisonMode (
    spectrumviewer::SpectrumComparisonMode mode) noexcept
{
    if (mode != spectrumviewer::SpectrumComparisonMode::absolute
        && mode != spectrumviewer::SpectrumComparisonMode::overlay
        && mode != spectrumviewer::SpectrumComparisonMode::deltaDb)
        return;
    comparisonMode.store (mode, std::memory_order_release);
    comparisonSettingsSequence.fetch_add (1, std::memory_order_release);
}

void SpectrumViewer::cancelSpectrumCapture()
{
    const auto state = captureState.load (std::memory_order_acquire);
    if (state == SpectrumCaptureState::live
        || state == SpectrumCaptureState::restoringLive)
        return;

    if (! acquisitionRunning.load (std::memory_order_acquire))
    {
        captureState.store (SpectrumCaptureState::live, std::memory_order_release);
        return;
    }

    captureState.store (SpectrumCaptureState::restoringLive,
                        std::memory_order_release);
    requestAnalysisConfiguration (false);
}

void SpectrumViewer::requestAnalysisConfiguration (bool forCapture)
{
    if (! acquisitionRunning.load (std::memory_order_acquire))
        return;

    const auto settings = getProfileSettings (
        forCapture ? SpectrumAnalysisProfile::fine
                   : analysisProfile.load (std::memory_order_relaxed));
    const auto windowSamples = std::round (acquisitionSampleRateHz * settings.windowSeconds);
    const auto hopSamples = forCapture
                                ? windowSamples
                                : std::round (acquisitionSampleRateHz * settings.hopSeconds);

    // Fixed profiles keep these dimensions bounded. Reject malformed host
    // metadata before it can create a pathological background allocation.
    constexpr double maximumWindowSamples = 1000000.0;
    if (! std::isfinite (windowSamples) || windowSamples < 1.0
        || windowSamples > maximumWindowSamples
        || ! std::isfinite (hopSamples) || hopSamples < 1.0
        || hopSamples > windowSamples)
    {
        configurationPending.store (false, std::memory_order_release);
        configurationFailures.fetch_add (1, std::memory_order_relaxed);
        if (forCapture)
            captureState.store (SpectrumCaptureState::failed,
                                std::memory_order_release);
        if (! hasActiveAnalysis())
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
        return;
    }

    spectrumviewer::SpectrumAnalysisPreparationRequest request;
    request.parameters.channelCount = acquisitionChannelCount;
    request.parameters.windowSampleCount = static_cast<std::size_t> (windowSamples);
    request.parameters.hopSampleCount = static_cast<std::size_t> (hopSamples);
    request.parameters.maximumInputBlockSampleCount = acquisitionMaximumInputBlockSamples;
    request.parameters.sampleRateHz = acquisitionSampleRateHz;
    request.parameters.timeHalfBandwidth = settings.timeHalfBandwidth;
    request.parameters.taperCount = settings.taperCount;
    request.parameters.detrendMode = spectrumviewer::DetrendMode::mean;
    request.parameters.generation = nextConfigurationGeneration++;
    request.outputQueueCapacity = OUTPUT_QUEUE_CAPACITY;
    if (forCapture)
    {
        request.captureId = requestedCaptureId.load (std::memory_order_acquire);
        request.captureTargetWindowCount = captureTargetWindows.load (
            std::memory_order_acquire);
    }
    request.sourceChannelIndices.assign (
        acquisitionChannels.begin(),
        acquisitionChannels.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));
    request.sourceChannelUnits.assign (
        acquisitionChannelUnits.begin(),
        acquisitionChannelUnits.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));

    requestedConfigurationGeneration.store (request.parameters.generation,
                                            std::memory_order_release);
    configurationPending.store (true, std::memory_order_release);
    if (! hasActiveAnalysis())
        analysisReadiness.store (SpectrumAnalysisReadiness::preparing, std::memory_order_release);
    else
        analysisReadiness.store (SpectrumAnalysisReadiness::live, std::memory_order_release);
    asynchronousAnalysis.request (std::move (request));
}
