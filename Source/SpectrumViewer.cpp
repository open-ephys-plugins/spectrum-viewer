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

#include <algorithm>
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

class ScopedAudioCallback final
{
public:
    explicit ScopedAudioCallback (std::atomic<std::size_t>& callbackCount) noexcept
        : count (callbackCount)
    {
        // Sequentially consistent, and paired with the seq_cst store/load in
        // stopAcquisition(). Acquire-release alone would leave a store-buffering
        // hole: the stopper could observe zero callbacks while this callback
        // still observes acquisitionRunning == true, and then free the FIFO
        // underneath it. Only a total order over both pairs excludes that.
        count.fetch_add (1, std::memory_order_seq_cst);
    }

    ~ScopedAudioCallback()
    {
        count.fetch_sub (1, std::memory_order_seq_cst);
    }

private:
    std::atomic<std::size_t>& count;
};
} // namespace

void SpectrumViewer::TransportDiagnostics::reset() noexcept
{
    for (auto* counter : { &droppedInputBlocks, &droppedInputSamples, &rejectedInputBlocks,
                           &invalidMappedInputBlocks, &invalidWorkerInputBlocks,
                           &inputDiscontinuities, &failedSpectrumWindows, &shedSpectrumWindows,
                           &unconfiguredInputBlocks, &unconfiguredInputSamples,
                           &staleConfigurationBlocks, &configurationFailures,
                           &droppedReferenceRequests })
        counter->store (0, std::memory_order_relaxed);
}

void SpectrumViewer::CaptureRuntimeState::resetProgress() noexcept
{
    // Release, so a reader that sees zero windows also sees the counters
    // below it cleared rather than left over from the previous capture.
    includedWindows.store (0, std::memory_order_release);
    wallSpanSeconds.store (0.0, std::memory_order_relaxed);
    failedWindows.store (0, std::memory_order_relaxed);
    shedWindows.store (0, std::memory_order_relaxed);
    discontinuities.store (0, std::memory_order_relaxed);
}

void SpectrumViewer::CaptureRuntimeState::resetWindowTracking() noexcept
{
    firstSample = 0;
    lastSampleExclusive = 0;
    lastFrameSequence = 0;
    lastPublishedDisplaySettings = 0;
    lastPublishedComparisonSettings = 0;
    hasFirstSample = false;
    completionPending = false;
}

void SpectrumViewer::CaptureRuntimeState::reset() noexcept
{
    // Withdraw the promise before releasing what it promised: retainedId
    // nonzero means `completed` exists.
    retainedId.store (0, std::memory_order_release);
    completed.reset();
    resetProgress();
    resetWindowTracking();
    targetWindows.store (0, std::memory_order_relaxed);
    windowSeconds.store (0.0, std::memory_order_relaxed);
    state.store (SpectrumCaptureState::live, std::memory_order_release);
}

SpectrumViewer::SpectrumViewer (
    spectrumviewer::AsyncSpectrumAnalysis::Builder configurationBuilder)
    : GenericProcessor ("Spectrum Viewer"),
      Thread ("FFT Thread"),
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
    acquisitionRunning.store (false, std::memory_order_seq_cst);
    activeConfigurationGeneration.store (0, std::memory_order_release);
    activeBinWidthHz.store (0.0f, std::memory_order_release);

    // Destruction cannot continue while a callback or worker may still refer
    // to member storage. Unlike stopThread(timeout), this never force-kills a
    // thread that may own locks or partially updated state. seq_cst for the
    // same reason as stopAcquisition().
    while (activeAudioCallbacks.load (std::memory_order_seq_cst) != 0)
        Thread::sleep (1);
    signalThreadShouldExit();
    notify();
    waitForThreadToExit (-1);

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
                                false);

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
        {
            if (acquisitionRunning.load (std::memory_order_acquire))
                rejectInputRouteReplacement();
            return;
        }

        LOGC ("Setting active stream to: ", streamKey);

        auto* stream = getDataStream (streamKey);
        if (stream == nullptr)
        {
            LOGD ("Spectrum Viewer ignored unavailable stream: ", streamKey);
            channels.clear();
            if (acquisitionRunning.load (std::memory_order_acquire))
                rejectInputRouteReplacement();
            return;
        }

        activeStream = stream->getStreamId();
        tfrParams.Fs = stream->getSampleRate();
        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        auto* p = dynamic_cast<SelectedChannelsParameter*> (stream->getParameter ("Channels"));
        if (p == nullptr)
        {
            if (acquisitionRunning.load (std::memory_order_acquire))
                rejectInputRouteReplacement();
            return;
        }

        channels = p->getArrayValue();
        if (acquisitionRunning.load (std::memory_order_acquire))
        {
            if (updateRequestedInputRoute (stream))
                requestInputRouteReplacement();
            else
                rejectInputRouteReplacement();
        }
        else if (auto* currentEditor = getEditor())
        {
            currentEditor->updateVisualizer();
        }
    }
    else if (param->getName() == "Channels")
    {
        // "Channels" is stream-scoped, so every stream owns one and every one of
        // them reports here. Only the displayed stream's selection is ours;
        // acting on another stream's would route its channel list to the wrong
        // runtime. Session loading delivers these in arbitrary order.
        if (param->getStreamId() != activeStream)
            return;

        channels.clear();

        auto* p = dynamic_cast<SelectedChannelsParameter*> (param);
        if (p == nullptr)
            return;

        channels = p->getArrayValue();

        if (acquisitionRunning.load (std::memory_order_acquire))
        {
            if (updateRequestedInputRoute (getDataStream (activeStream)))
                requestInputRouteReplacement();
            else
                rejectInputRouteReplacement();
        }

        if (! acquisitionRunning.load (std::memory_order_acquire))
        {
            if (auto* currentEditor = getEditor())
                currentEditor->updateVisualizer();
        }
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

bool SpectrumViewer::tryReadDisplaySettings (DisplaySettings& settings) const noexcept
{
    // Bounded like readInputRoute(): the writer is the message thread, which
    // can issue a burst of updates (CanvasPlot::resized() publishes a column
    // count on every resize event), and an unbounded spin here would stall
    // frame publication on the worker.
    for (int attempt = 0; attempt < seqlockReadAttempts; ++attempt)
    {
        const auto before = displaySettingsSequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue;

        settings.columnCount = displayColumnCount.load (std::memory_order_relaxed);
        settings.frequencyScale = displayFrequencyScale.load (std::memory_order_relaxed);
        settings.aperiodicMode = aperiodicDisplayMode.load (std::memory_order_relaxed);
        settings.minimumFrequencyHz = displayMinimumFrequencyHz.load (std::memory_order_relaxed);
        settings.maximumFrequencyHz = displayMaximumFrequencyHz.load (std::memory_order_relaxed);

        // The payload loads above are relaxed, and an acquire load on the
        // sequence would only stop *later* accesses from being hoisted above
        // it. This fence is what stops those relaxed loads from being sunk
        // below the re-read, which is the actual torn-read hazard.
        std::atomic_thread_fence (std::memory_order_acquire);

        if (displaySettingsSequence.load (std::memory_order_relaxed) == before)
        {
            settings.sequence = before;
            return true;
        }
    }
    return false;
}


void SpectrumViewer::publishInputRoute (std::uint16_t streamId,
                                        std::size_t channelCount,
                                        const int* globalChannelIndices,
                                        std::uint64_t generation) noexcept
{
    inputRouteSequence.fetch_add (1, std::memory_order_acq_rel);
    publishedInputStream.store (streamId, std::memory_order_relaxed);
    publishedInputChannelCount.store (channelCount, std::memory_order_relaxed);
    for (std::size_t channel = 0; channel < MAX_CHANS; ++channel)
    {
        const auto globalChannel = globalChannelIndices != nullptr && channel < channelCount
                                       ? globalChannelIndices[channel]
                                       : -1;
        publishedGlobalChannels[channel].store (globalChannel,
                                                std::memory_order_relaxed);
    }
    publishedInputGeneration.store (generation, std::memory_order_relaxed);
    inputRouteSequence.fetch_add (1, std::memory_order_release);
    activeConfigurationGeneration.store (generation, std::memory_order_release);
}

bool SpectrumViewer::readInputRoute (InputRouteSnapshot& route) const noexcept
{
    for (int attempt = 0; attempt < seqlockReadAttempts; ++attempt)
    {
        const auto before = inputRouteSequence.load (std::memory_order_acquire);
        if ((before & 1u) != 0u)
            continue;

        route.streamId = publishedInputStream.load (std::memory_order_relaxed);
        route.channelCount = publishedInputChannelCount.load (std::memory_order_relaxed);
        for (std::size_t channel = 0; channel < MAX_CHANS; ++channel)
            route.globalChannelIndices[channel] = publishedGlobalChannels[channel].load (
                std::memory_order_relaxed);
        route.generation = publishedInputGeneration.load (std::memory_order_relaxed);

        // Stops the relaxed payload loads above from being sunk below the
        // re-read. An acquire load on the sequence would only constrain
        // accesses that follow it, which is the wrong direction here. This
        // payload is eleven words wide, so a torn read is a real hazard.
        std::atomic_thread_fence (std::memory_order_acquire);

        if (inputRouteSequence.load (std::memory_order_relaxed) == before)
            return route.channelCount <= MAX_CHANS;
    }
    return false;
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
    else if (capture.completed != nullptr
             && capture.completed->getCaptureId() == request)
    {
        spectrumReference = capture.completed;
        referenceCapturedAtMilliseconds.store (
            capture.completed->getCapturedAtUnixMilliseconds(),
            std::memory_order_relaxed);
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::compatible,
            std::memory_order_relaxed);
        referenceCaptureId.store (request, std::memory_order_release);
    }
    else
    {
        // The named capture is not retained: finalizeCapturedSpectrum() could
        // not allocate it, or the runtime holding it was released first. The
        // exchange above has already consumed the request, so without this
        // counter the UI would keep showing the reference controls it enabled
        // on the click and never learn that nothing was set.
        diagnostics.droppedReferenceRequests.fetch_add (1, std::memory_order_release);
        LOGE ("Spectrum Viewer could not set capture ", request,
              " as the reference because it was not retained");
    }

    capture.completionPending = capture.state.load (std::memory_order_relaxed)
                               == SpectrumCaptureState::frozen;
}

bool SpectrumViewer::finalizeCapturedSpectrum()
{
    if (activeAnalysis == nullptr || ! activeAnalysis->isCaptureRuntime())
        return false;
    if (capture.completed != nullptr)
        return true;

    auto* accumulator = activeAnalysis->getCaptureAccumulator();
    if (accumulator == nullptr || ! accumulator->isComplete())
        return false;

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
        quality.firstSample = capture.firstSample;
        quality.lastSampleExclusive = capture.lastSampleExclusive;
        quality.failedWindowCount = pipeline.getFailedWindowCount()
                                    + accumulator->getRejectedWindowCount();
        quality.shedWindowCount = pipeline.getShedWindowCount();
        quality.discontinuityCount = pipeline.getDiscontinuityCount();

        const auto& frameFifo = activeAnalysis->getFrameFifo();
        capture.completed = std::make_shared<const spectrumviewer::CapturedSpectrum> (
            activeAnalysis->getCaptureId(),
            Time::currentTimeMillis(),
            activeAnalysis->getConfiguration().getFrameDescriptor(),
            frameFifo.getSourceChannelIndices(),
            frameFifo.getSourceChannelUnits(),
            std::move (mean),
            std::move (variance),
            quality,
            frameFifo.getSourceStreamId());
    }
    catch (const std::exception& error)
    {
        LOGE ("Unable to retain completed spectrum capture: ", error.what(),
              " (windows ", accumulator->getIncludedWindowCount(), "/",
              accumulator->getTargetWindowCount(),
              ", channels ", accumulator->getChannelCount(),
              ", bins ", accumulator->getBinCount(),
              ", samples ", capture.firstSample, " to ", capture.lastSampleExclusive, ")");
        return false;
    }

    // Published only after the capture exists, so a nonzero id read anywhere
    // else is a promise that setCurrentCaptureAsReference() can be honoured.
    capture.retainedId.store (capture.completed->getCaptureId(),
                             std::memory_order_release);
    return true;
}

bool SpectrumViewer::publishReducedSpectrum (
    const float* planarPsd,
    std::size_t channelCount,
    std::size_t binCount,
    const spectrumviewer::SpectrumFrameDescriptor& descriptor,
    std::int64_t firstSample,
    std::uint64_t sequence,
    spectrumviewer::SpectrumCaptureFrameStatus captureStatus) noexcept
{
    if (activeAnalysis == nullptr)
        return false;
    auto& frameFifo = activeAnalysis->getFrameFifo();
    if (frameFifo.getNumReady() >= frameFifo.getCapacity())
        return false;

    // Reducing against a torn snapshot would publish a frame whose axis does
    // not match its data. Frames are replaceable latest-state: skipping one is
    // free, and the next window arrives a hop later.
    DisplaySettings settings;
    if (! tryReadDisplaySettings (settings))
        return false;

    auto maximumHz = settings.maximumFrequencyHz;
    if (maximumHz <= 0.0)
        maximumHz = descriptor.sampleRateHz * 0.5;
    auto minimumHz = settings.minimumFrequencyHz;
    if (settings.frequencyScale == spectrumviewer::FrequencyScale::logarithmic
        && minimumHz <= 0.0)
        minimumHz = std::min (10.0, maximumHz / 10.0);

    auto& reducer = activeAnalysis->getDisplayReducer();
    if (! reducer.reduce (planarPsd,
                          channelCount,
                          binCount,
                          descriptor.sampleRateHz,
                          descriptor.windowSampleCount,
                          settings.columnCount,
                          settings.frequencyScale,
                          minimumHz,
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
            frameFifo.getSourceChannelUnits(),
            frameFifo.getSourceStreamId());
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
                        minimumHz,
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
    auto* baselineDb = settings.aperiodicMode
                               == spectrumviewer::AperiodicDisplayMode::off
                           ? nullptr
                           : activeAnalysis->getBaselineScratch();
    if (baselineDb != nullptr
        && ! activeAnalysis->getBaselineEstimator().estimate (
            planarPsd,
            channelCount,
            binCount,
            descriptor.sampleRateHz,
            descriptor.windowSampleCount,
            reduced.frequenciesHz,
            reduced.numColumns,
            baselineDb))
        baselineDb = nullptr;
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
                                     captureStatus,
                                     comparisonData,
                                     comparison,
                                     baselineDb);
}

bool SpectrumViewer::publishCapturedSpectrum (bool complete) noexcept
{
    if (activeAnalysis == nullptr || ! activeAnalysis->isCaptureRuntime())
        return false;
    auto* accumulator = activeAnalysis->getCaptureAccumulator();
    if (accumulator == nullptr || accumulator->getIncludedWindowCount() == 0
        || ! capture.hasFirstSample)
        return false;
    const auto& descriptor = activeAnalysis->getConfiguration().getFrameDescriptor();

    spectrumviewer::SpectrumCaptureFrameStatus status;
    status.product = complete ? spectrumviewer::SpectrumFrameProduct::captureComplete
                              : spectrumviewer::SpectrumFrameProduct::captureProgress;
    status.captureId = activeAnalysis->getCaptureId();
    status.includedWindowCount = accumulator->getIncludedWindowCount();
    status.targetWindowCount = accumulator->getTargetWindowCount();
    status.lastSampleExclusive = capture.lastSampleExclusive;
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
        capture.firstSample,
        capture.lastFrameSequence,
        status);
    if (published)
    {
        // publishReducedSpectrum only succeeds on a stable snapshot, so this
        // read cannot legitimately fail. If it does, leave the watermark alone
        // so the frozen capture is republished on the next pass rather than
        // recorded against a sequence that was never used.
        DisplaySettings settings;
        if (tryReadDisplaySettings (settings))
        {
            capture.lastPublishedDisplaySettings = settings.sequence;
            capture.lastPublishedComparisonSettings = comparisonSettingsSequence.load (
                std::memory_order_acquire);
        }
    }
    return published;
}

void SpectrumViewer::process (AudioBuffer<float>& continuousBuffer)
{
    ScopedAudioCallback callback (activeAudioCallbacks);
    // seq_cst, paired with stopAcquisition(). See ScopedAudioCallback.
    if (! acquisitionRunning.load (std::memory_order_seq_cst))
        return;

    auto* fifo = inputFifo.get();
    if (fifo == nullptr)
        return;

    InputRouteSnapshot route;
    if (! readInputRoute (route))
    {
        diagnostics.invalidMappedInputBlocks.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // An empty route is not a mapping error - it is the worker saying no
    // runtime claims this data, either before the first configuration or after
    // it released a route whose selection became invalid. Counting these as
    // rejected blocks would make that diagnostic climb for as long as a user
    // leaves nothing selected. The sample count is unavailable here:
    // getNumSamplesInBlock() throws for the zero stream id an empty route
    // carries, so it must stay below this guard.
    if (route.channelCount == 0)
    {
        diagnostics.unconfiguredInputBlocks.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const auto incomingSampleCount = static_cast<std::size_t> (
        getNumSamplesInBlock (route.streamId));
    if (incomingSampleCount == 0)
        return;

    if (route.generation == 0)
    {
        diagnostics.unconfiguredInputBlocks.fetch_add (1, std::memory_order_relaxed);
        diagnostics.unconfiguredInputSamples.fetch_add (incomingSampleCount, std::memory_order_relaxed);
        return;
    }

    std::array<const float*, MAX_CHANS> channelData {};

    for (std::size_t channel = 0; channel < route.channelCount; ++channel)
    {
        const auto globalChannel = route.globalChannelIndices[channel];
        if (globalChannel < 0 || globalChannel >= continuousBuffer.getNumChannels())
        {
            diagnostics.invalidMappedInputBlocks.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        channelData[channel] = continuousBuffer.getReadPointer (globalChannel);
    }

    if (! fifo->tryPush (channelData.data(),
                         route.channelCount,
                         incomingSampleCount,
                         getFirstSampleNumberForBlock (route.streamId),
                         route.generation))
    {
        diagnostics.droppedInputBlocks.store (fifo->getDroppedBlockCount(), std::memory_order_relaxed);
        diagnostics.droppedInputSamples.store (fifo->getDroppedSampleCount(), std::memory_order_relaxed);
        diagnostics.rejectedInputBlocks.store (fifo->getRejectedBlockCount(), std::memory_order_relaxed);
    }
}

void SpectrumViewer::run()
{
    while (! threadShouldExit())
    {
        // Before adopting: a rejected route replacement bumps the requested
        // generation, so any result still in flight is discarded here anyway.
        discardInvalidatedInputRoute();
        adoptPreparedAnalysis();
        applyReferenceRequest();
        republishFrozenCapture();
        if (! processQueuedInput())
            waitForInput();
    }
}

void SpectrumViewer::republishFrozenCapture() noexcept
{
    // A frozen capture receives no new windows, but it still has to be reduced
    // again whenever the display settings or the comparison change.
    if (activeAnalysis == nullptr || ! activeAnalysis->isCaptureRuntime()
        || capture.state.load (std::memory_order_acquire) != SpectrumCaptureState::frozen)
        return;

    const auto displayVersion = displaySettingsSequence.load (std::memory_order_acquire);
    const auto comparisonVersion = comparisonSettingsSequence.load (
        std::memory_order_acquire);
    const auto& frameFifo = activeAnalysis->getFrameFifo();
    if ((displayVersion & 1u) == 0u
        && (capture.completionPending
            || displayVersion != capture.lastPublishedDisplaySettings
            || comparisonVersion != capture.lastPublishedComparisonSettings)
        && frameFifo.getNumReady() < frameFifo.getCapacity())
        capture.completionPending = ! publishCapturedSpectrum (true);
}

bool SpectrumViewer::processQueuedInput()
{
    auto* fifo = inputFifo.get();
    if (fifo == nullptr || activeAnalysis == nullptr)
        return false;

    bool consumedBlock = false;
    spectrumviewer::BacklogSheddingPolicy sheddingPolicy;
    while (! threadShouldExit())
    {
        // A replacement can finish between blocks, and the next block belongs
        // to it rather than to the runtime it replaced.
        adoptPreparedAnalysis();
        if (activeAnalysis == nullptr)
            break;

        if (activeAnalysis->isCaptureRuntime()
            && capture.state.load (std::memory_order_acquire)
                   != SpectrumCaptureState::capturing)
        {
            if (! drainHeldCaptureBlock (*fifo))
                break;
            consumedBlock = true;
            continue;
        }

        auto& pipeline = activeAnalysis->getPipeline();
        spectrumviewer::SpectrumAnalysisPipeline::AppendResult appended;
        const auto appendBlock = [&] (const auto& block)
        {
            std::array<const float*, MAX_CHANS> channelData {};
            for (std::size_t channel = 0; channel < block.numChannels; ++channel)
                channelData[channel] = block.getChannelData (channel);

            // appendBlock copies the complete block. Returning from this
            // callback releases the FIFO slot before any FFT work begins.
            appended = pipeline.appendBlock (channelData.data(),
                                             block.numChannels,
                                             block.numSamples,
                                             block.firstSample,
                                             block.configurationGeneration);
        };
        if (! fifo->tryPop (appendBlock))
            break;
        consumedBlock = true;

        // Chosen before the block is judged: the policy tracks the queue, and a
        // rejected block was dequeued like any other.
        const auto sheddingAction = chooseSheddingAction (sheddingPolicy, *fifo);
        if (appended.status != spectrumviewer::SpectrumAnalysisPipeline::AppendStatus::accepted)
        {
            recordRejectedBlock (appended.status);
            continue;
        }

        if (appended.discontinuity)
            diagnostics.inputDiscontinuities.store (pipeline.getDiscontinuityCount(),
                                                    std::memory_order_relaxed);

        if (! shedObsoleteWindows (sheddingAction))
            continue;

        publishReadyFrames();
        updateWindowCounters();
        trackWarmup (appended);
    }
    return consumedBlock;
}

bool SpectrumViewer::drainHeldCaptureBlock (spectrumviewer::SampleBlockFifo& fifo)
{
    // Nothing analyses input while a capture is frozen on screen or the live
    // runtime is still being built, but the queue has to keep moving or the
    // callback would start dropping blocks.
    return fifo.tryPop ([] (const auto&) {});
}

spectrumviewer::BacklogSheddingPolicy::Action SpectrumViewer::chooseSheddingAction (
    spectrumviewer::BacklogSheddingPolicy& policy,
    spectrumviewer::SampleBlockFifo& fifo)
{
    // Shedding is only sound for live display frames, which are replaceable
    // latest-state. A capture window is 2 s of data the average will never see
    // again, so discarding one does not make the capture catch up - it makes it
    // stall. Back-pressure for a capture is the input queue alone.
    if (activeAnalysis->isCaptureRuntime())
    {
        policy.reset();
        return spectrumviewer::BacklogSheddingPolicy::Action::processAll;
    }
    return policy.inputBlockDequeued (fifo.getNumReady() > 0);
}

void SpectrumViewer::recordRejectedBlock (
    spectrumviewer::SpectrumAnalysisPipeline::AppendStatus status)
{
    if (status == spectrumviewer::SpectrumAnalysisPipeline::AppendStatus::configurationMismatch)
    {
        diagnostics.staleConfigurationBlocks.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    // LOGE takes a global lock and writes to two streams, and a systematic
    // shape mismatch would hit this for every block. The counter carries the
    // rate; the log only needs to say it is happening, so it thins out to
    // powers of two.
    const auto rejected = diagnostics.invalidWorkerInputBlocks.fetch_add (
                              1, std::memory_order_relaxed)
                          + 1;
    if ((rejected & (rejected - 1)) == 0)
        LOGE ("Spectrum Viewer worker rejected an invalid FIFO block (",
              rejected, " so far)");
}

bool SpectrumViewer::shedObsoleteWindows (spectrumviewer::BacklogSheddingPolicy::Action action)
{
    using Action = spectrumviewer::BacklogSheddingPolicy::Action;
    if (action == Action::processAll)
        return true;

    auto& pipeline = activeAnalysis->getPipeline();
    const auto shed = action == Action::discardAll
                          ? pipeline.discardReadyFrames()
                          : pipeline.discardReadyFramesExceptLatest();
    diagnostics.shedSpectrumWindows.fetch_add (shed, std::memory_order_relaxed);
    return action != Action::discardAll;
}

void SpectrumViewer::publishReadyFrames()
{
    const auto publishFrame = [this] (const auto& frame)
    {
        if (activeAnalysis->isCaptureRuntime())
        {
            addCaptureWindow (frame);
            return;
        }

        publishReducedSpectrum (frame.getChannelData (0),
                                frame.numChannels,
                                frame.numBins,
                                frame.descriptor,
                                frame.firstSample,
                                frame.sequence);
    };

    // A capture takes exactly its target number of windows.
    auto maximumFrames = std::numeric_limits<std::size_t>::max();
    if (activeAnalysis->isCaptureRuntime())
    {
        const auto* accumulator = activeAnalysis->getCaptureAccumulator();
        maximumFrames = accumulator->getTargetWindowCount()
                        - accumulator->getIncludedWindowCount();
    }
    activeAnalysis->getPipeline().consumeReadyFrames (publishFrame, maximumFrames);
}

void SpectrumViewer::addCaptureWindow (
    const spectrumviewer::SpectrumAnalysisPipeline::FrameView& frame)
{
    auto* accumulator = activeAnalysis->getCaptureAccumulator();
    if (accumulator == nullptr
        || ! accumulator->add (frame.getChannelData (0),
                               frame.numChannels,
                               frame.numBins))
        return;

    // The analyzed span is anchored on included windows, not on arriving
    // blocks: CapturedSpectrum rejects a span that does not run forward, and a
    // discontinuity can move the source's sample numbering backwards.
    const auto windowEndSample =
        frame.firstSample
        + static_cast<std::int64_t> (frame.descriptor.windowSampleCount);
    if (! capture.hasFirstSample
        || frame.firstSample < capture.firstSample)
    {
        capture.firstSample = frame.firstSample;
        capture.lastSampleExclusive = windowEndSample;
        capture.hasFirstSample = true;
    }
    else
        capture.lastSampleExclusive =
            std::max (capture.lastSampleExclusive, windowEndSample);
    capture.lastFrameSequence = frame.sequence;
    capture.includedWindows.store (accumulator->getIncludedWindowCount(),
                                   std::memory_order_release);
    // Spanned against analyzed seconds is the stall signal: non-overlapping
    // windows make them equal unless data was lost between them.
    capture.wallSpanSeconds.store (
        static_cast<double> (capture.lastSampleExclusive - capture.firstSample)
            / frame.descriptor.sampleRateHz,
        std::memory_order_relaxed);

    const auto complete = accumulator->isComplete();
    const auto retained = complete && finalizeCapturedSpectrum();
    const auto published = (! complete || retained)
                           && publishCapturedSpectrum (complete);
    if (complete)
    {
        capture.completionPending = retained && ! published;
        // Freezing is what enables the reference controls. Without a retained
        // capture there is nothing behind them, so report the failure now
        // instead of silently dropping the request after the click.
        capture.state.store (retained ? SpectrumCaptureState::frozen
                                      : SpectrumCaptureState::failed,
                             std::memory_order_release);
    }
}

void SpectrumViewer::updateWindowCounters() noexcept
{
    auto& pipeline = activeAnalysis->getPipeline();
    diagnostics.failedSpectrumWindows.store (pipeline.getFailedWindowCount(),
                                             std::memory_order_relaxed);
    if (! activeAnalysis->isCaptureRuntime())
        return;

    capture.failedWindows.store (
        pipeline.getFailedWindowCount()
            + activeAnalysis->getCaptureAccumulator()->getRejectedWindowCount(),
        std::memory_order_relaxed);
    capture.shedWindows.store (pipeline.getShedWindowCount(), std::memory_order_relaxed);
    capture.discontinuities.store (pipeline.getDiscontinuityCount(),
                                   std::memory_order_relaxed);
}

void SpectrumViewer::trackWarmup (
    const spectrumviewer::SpectrumAnalysisPipeline::AppendResult& appended) noexcept
{
    if (analysisReadiness.load (std::memory_order_relaxed)
        != SpectrumAnalysisReadiness::warmingUp)
        return;

    warmupSampleCount.store (activeAnalysis->getPipeline().getBufferedSampleCount(),
                             std::memory_order_relaxed);
    if (appended.windowsReady > 0)
        analysisReadiness.store (SpectrumAnalysisReadiness::live, std::memory_order_release);
}

void SpectrumViewer::waitForInput()
{
    // Thread::notify() takes a mutex in JUCE 8, so the audio callback must not
    // use it. A short worker-only timed wait avoids both callback locks and spin.
    wait (2.0);
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
        diagnostics.configurationFailures.fetch_add (1, std::memory_order_relaxed);
        if (result.captureId != 0)
        {
            // Only a retained capture may report frozen; see the capture
            // completion path in run().
            const auto retained =
                activeAnalysis != nullptr && activeAnalysis->isCaptureRuntime()
                && capture.retainedId.load (std::memory_order_acquire) != 0;
            capture.state.store (retained ? SpectrumCaptureState::frozen
                                         : SpectrumCaptureState::failed,
                                std::memory_order_release);
        }
        else if (capture.state.load (std::memory_order_relaxed)
                 == SpectrumCaptureState::restoringLive)
            capture.state.store (capture.replacementFailureState.load (
                                    std::memory_order_relaxed),
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

    capture.resetWindowTracking();
    capture.lastPublishedDisplaySettings = displaySettingsSequence.load (std::memory_order_acquire);
    capture.lastPublishedComparisonSettings = comparisonSettingsSequence.load (
        std::memory_order_acquire);
    capture.resetProgress();
    if (activeAnalysis->isCaptureRuntime())
    {
        capture.completed.reset();
        capture.retainedId.store (0, std::memory_order_release);
        const auto& descriptor = activeAnalysis->getConfiguration().getFrameDescriptor();
        capture.windowSeconds.store (
            static_cast<double> (descriptor.windowSampleCount)
                / descriptor.sampleRateHz,
            std::memory_order_relaxed);
        capture.requestedId.store (activeAnalysis->getCaptureId(), std::memory_order_release);
        capture.targetWindows.store (
            activeAnalysis->getCaptureAccumulator()->getTargetWindowCount(),
            std::memory_order_release);
        capture.state.store (SpectrumCaptureState::capturing, std::memory_order_release);
    }
    else
    {
        capture.targetWindows.store (0, std::memory_order_release);
        capture.state.store (SpectrumCaptureState::live, std::memory_order_release);
    }

    // Publication of the generation is the callback's permission to enqueue
    // blocks for this runtime. Older queued blocks are rejected at the worker
    // boundary instead of being mixed into the new history.
    const auto& globalChannels = activeAnalysis->getSourceGlobalChannelIndices();
    publishInputRoute (activeAnalysis->getSourceStreamId(),
                       globalChannels.size(),
                       globalChannels.data(),
                       result.generation);
}

void SpectrumViewer::updateSettings()
{
    // Resolve the processor-scoped selection before consulting the cached
    // stream id. Session loading can update a parameter without delivering
    // its callback before the first acquisition starts.
    if (auto* selectedStream = getParameter ("active_stream"))
        parameterValueChanged (selectedStream);

    if (dataStreams.size() > 0)
    {
        if (auto* stream = getDataStream (activeStream))
        {
            if (auto* parameter = stream->getParameter ("Channels"))
            {
                parameterValueChanged (parameter);
                return;
            }
        }
    }

    channels.clear();
}

Array<int> SpectrumViewer::getActiveChans()
{
    return channels;
}

const String SpectrumViewer::getChanName (int localIdx)
{
    return getChanName (activeStream, localIdx);
};

const String SpectrumViewer::getChanName (std::uint16_t streamId, int localIdx)
{
    auto* stream = getDataStream (streamId);
    if (stream == nullptr
        || ! isPositiveAndBelow (localIdx, stream->getContinuousChannels().size()))
        return "Channel " + String (localIdx + 1);

    auto* channel = stream->getContinuousChannels()[localIdx];
    return channel != nullptr ? channel->getName()
                              : "Channel " + String (localIdx + 1);
};

bool SpectrumViewer::updateRequestedInputRoute (DataStream* stream)
{
    if (stream == nullptr)
        return false;

    const auto& streamChannels = stream->getContinuousChannels();
    const auto channelCount = static_cast<std::size_t> (
        std::min (channels.size(), MAX_CHANS));
    const auto sampleRate = static_cast<double> (stream->getSampleRate());
    if (channelCount == 0 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
        return false;

    std::array<int, MAX_CHANS> localChannels {};
    std::array<int, MAX_CHANS> globalChannels {};
    std::array<std::string, MAX_CHANS> channelUnits {};
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto localChannel = channels[static_cast<int> (channel)];
        if (! isPositiveAndBelow (localChannel, streamChannels.size())
            || streamChannels[localChannel] == nullptr)
        {
            LOGE ("Unable to configure Spectrum Viewer with invalid channel index ",
                  localChannel);
            return false;
        }

        const auto globalChannel = getGlobalChannelIndex (stream->getStreamId(),
                                                          localChannel);
        if (globalChannel < 0)
        {
            LOGE ("Unable to map Spectrum Viewer channel ", localChannel);
            return false;
        }

        localChannels[channel] = localChannel;
        globalChannels[channel] = globalChannel;
        channelUnits[channel] = streamChannels[localChannel]->getUnits().toStdString();
    }

    acquisitionChannels = std::move (localChannels);
    acquisitionGlobalChannels = std::move (globalChannels);
    acquisitionChannelUnits = std::move (channelUnits);
    acquisitionChannelCount = channelCount;
    acquisitionStream = stream->getStreamId();
    acquisitionSampleRateHz = sampleRate;
    return true;
}

void SpectrumViewer::requestInputRouteReplacement()
{
    // A routable selection supersedes a pending invalidation. Clearing the
    // command before requesting keeps the worker from tearing down the route
    // that the configuration requested below is about to replace.
    inputRouteInvalidated.store (false, std::memory_order_release);

    const auto state = capture.state.load (std::memory_order_acquire);
    if (state != SpectrumCaptureState::live
        && state != SpectrumCaptureState::failed)
    {
        capture.replacementFailureState.store (
            state == SpectrumCaptureState::capturing
                ? SpectrumCaptureState::capturing
                : state == SpectrumCaptureState::frozen
                      ? SpectrumCaptureState::frozen
                      : SpectrumCaptureState::live,
            std::memory_order_relaxed);
        capture.state.store (SpectrumCaptureState::restoringLive,
                            std::memory_order_release);
    }
    requestAnalysisConfiguration (false);
}

void SpectrumViewer::rejectInputRouteReplacement() noexcept
{
    // Supersede any preparation already in flight so its result is discarded
    // on arrival rather than installed against a selection that is gone.
    requestedConfigurationGeneration.store (nextConfigurationGeneration++,
                                            std::memory_order_release);
    configurationPending.store (false, std::memory_order_release);

    // Keeping the live runtime here would leave the canvas drawing, and the
    // legend naming, the channels the user just deselected - data that is
    // still arriving but no longer describes the selection. Release it.
    //
    // The teardown has to happen on the worker: it is the input route
    // seqlock's only writer while acquisition runs, and it owns activeAnalysis.
    // So this is a command, not the act itself.
    inputRouteInvalidated.store (true, std::memory_order_release);
    analysisReadiness.store (SpectrumAnalysisReadiness::invalidSelection,
                             std::memory_order_release);
}

void SpectrumViewer::discardInvalidatedInputRoute() noexcept
{
    if (! inputRouteInvalidated.exchange (false, std::memory_order_acq_rel))
        return;

    // Withdraw the callback's permission to enqueue before releasing the
    // runtime those blocks were destined for. Blocks already queued keep the
    // old generation and are rejected at the pipeline boundary if a later
    // runtime ever sees them.
    publishInputRoute (0, 0, nullptr, 0);
    activeBinWidthHz.store (0.0f, std::memory_order_release);

    if (activeAnalysis != nullptr)
    {
        {
            const std::lock_guard<std::mutex> lock (displayAnalysisMutex);
            displayAnalysis.reset();
        }
        asynchronousAnalysis.retire (activeAnalysis);
        activeAnalysis.reset();
    }

    warmupSampleCount.store (0, std::memory_order_relaxed);
    warmupTargetSampleCount.store (0, std::memory_order_relaxed);

    // A frozen capture is republished from the runtime's accumulator, so it
    // cannot outlive the runtime. The reference is a standalone snapshot and
    // does survive; its compatibility is re-evaluated against whatever
    // selection comes next.
    capture.reset();

    // The message thread can make the selection valid again between the
    // command and this teardown. That configuration request owns the readiness
    // state, so only claim it when nothing has superseded the invalidation.
    if (! configurationPending.load (std::memory_order_acquire))
        analysisReadiness.store (SpectrumAnalysisReadiness::invalidSelection,
                                 std::memory_order_release);
}

bool SpectrumViewer::startAcquisition()
{
    if (isEnabled)
    {
        if (isThreadRunning()
            || activeAudioCallbacks.load (std::memory_order_acquire) != 0)
        {
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
            LOGE ("Unable to start Spectrum Viewer while its previous worker or callback is active");
            return false;
        }

        if (inputFifo != nullptr || activeAnalysis != nullptr)
            clearAcquisitionState();

        // The first host start can precede the editor/visualizer update that
        // populates SelectedStreamParameter's display-name table. Refresh all
        // available routing state, then fall back to its selected index against
        // the processor's actual streams rather than requiring that UI table.
        updateSettings();

        auto* stream = getDataStream (activeStream);
        if (stream == nullptr && ! dataStreams.isEmpty())
        {
            auto selectedIndex = 0;
            if (auto* selected = dynamic_cast<SelectedStreamParameter*> (
                    getParameter ("active_stream")))
                selectedIndex = jlimit (0, dataStreams.size() - 1,
                                        selected->getSelectedIndex());
            stream = dataStreams[selectedIndex];
            if (stream != nullptr)
            {
                activeStream = stream->getStreamId();
                if (auto* selectedChannels = dynamic_cast<SelectedChannelsParameter*> (
                        stream->getParameter ("Channels")))
                    channels = selectedChannels->getArrayValue();
            }
        }
        if (! updateRequestedInputRoute (stream))
        {
            // Same cause as a rejected replacement: the selection is not
            // routable. Report it as such rather than as a broken analysis.
            analysisReadiness.store (SpectrumAnalysisReadiness::invalidSelection,
                                     std::memory_order_release);
            LOGE ("Unable to configure Spectrum Viewer without an active stream");
            return false;
        }

        const auto reportedBlockSize = getBlockSize();
        const auto maximumInputBlockSamples = std::max (
            MINIMUM_INPUT_BLOCK_CAPACITY,
            reportedBlockSize > 0 ? static_cast<std::size_t> (reportedBlockSize) : 0u);
        if (acquisitionChannelCount == 0
            || ! std::isfinite (acquisitionSampleRateHz)
            || acquisitionSampleRateHz <= 0.0)
        {
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
            return false;
        }

        try
        {
            inputFifo = std::make_unique<spectrumviewer::SampleBlockFifo> (
                MAX_CHANS,
                maximumInputBlockSamples,
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

        acquisitionMaximumInputBlockSamples = maximumInputBlockSamples;
        publishInputRoute (acquisitionStream,
                           acquisitionChannelCount,
                           acquisitionGlobalChannels.data(),
                           0);
        activeConfigurationGeneration.store (0, std::memory_order_release);
        activeBinWidthHz.store (0.0f, std::memory_order_release);
        diagnostics.reset();
        inputRouteInvalidated.store (false, std::memory_order_relaxed);
        // The worker is not running yet, so the capture's worker-only fields
        // are safe to reset from here.
        capture.reset();
        capture.requestedId.store (0, std::memory_order_relaxed);
        referenceRequest.store (0, std::memory_order_relaxed);
        referenceCaptureId.store (0, std::memory_order_relaxed);
        referenceCapturedAtMilliseconds.store (0, std::memory_order_relaxed);
        referenceCompatibility.store (
            spectrumviewer::SpectrumReferenceCompatibility::noReference,
            std::memory_order_relaxed);
        spectrumReference.reset();
        warmupSampleCount.store (0, std::memory_order_relaxed);
        warmupTargetSampleCount.store (0, std::memory_order_relaxed);
        analysisReadiness.store (SpectrumAnalysisReadiness::preparing, std::memory_order_release);
        acquisitionRunning.store (true, std::memory_order_release);
        if (! startThread (Thread::Priority::normal))
        {
            acquisitionRunning.store (false, std::memory_order_release);
            clearAcquisitionState();
            analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                     std::memory_order_release);
            LOGE ("Unable to start Spectrum Viewer analysis worker");
            return false;
        }
        requestAnalysisConfiguration();
    }
    return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
    acquisitionRunning.store (false, std::memory_order_seq_cst);
    activeConfigurationGeneration.store (0, std::memory_order_release);
    activeBinWidthHz.store (0.0f, std::memory_order_release);

    // Both this store/load pair and the callback's increment/load pair are
    // seq_cst, so they participate in one total order. That is what makes the
    // following argument sound: a callback that begins after the store above
    // observes false and returns before touching the FIFO, so a nonzero count
    // can only be an older callback that may still hold a pointer into the
    // current FIFO. With acquire/release alone, store-load reordering would
    // permit both sides to miss each other and clearAcquisitionState() would
    // free the FIFO under a live callback.
    if (activeAudioCallbacks.load (std::memory_order_seq_cst) != 0)
    {
        analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                 std::memory_order_release);
        LOGE ("Spectrum Viewer retained acquisition state because an audio callback was still active");
        return false;
    }

    if (! stopWorkerSafely (1000))
    {
        analysisReadiness.store (SpectrumAnalysisReadiness::configurationFailed,
                                 std::memory_order_release);
        LOGE ("Spectrum Viewer retained acquisition state because its worker did not stop in time");
        return false;
    }

    clearAcquisitionState();
    return true;
}

bool SpectrumViewer::stopWorkerSafely (int timeoutMilliseconds) noexcept
{
    signalThreadShouldExit();
    notify();
    return waitForThreadToExit (timeoutMilliseconds);
}

void SpectrumViewer::clearAcquisitionState()
{
    if (activeAnalysis != nullptr)
        asynchronousAnalysis.retire (activeAnalysis);
    {
        const std::lock_guard<std::mutex> lock (displayAnalysisMutex);
        displayAnalysis.reset();
    }
    activeAnalysis.reset();
    inputFifo.reset();
    publishInputRoute (0, 0, nullptr, 0);
    acquisitionChannelCount = 0;
    acquisitionStream = 0;
    acquisitionMaximumInputBlockSamples = 0;
    acquisitionSampleRateHz = 0.0;
    configurationPending.store (false, std::memory_order_release);
    // The worker is gone by the time this runs, so an armed teardown command
    // would otherwise survive to fire against the next acquisition.
    inputRouteInvalidated.store (false, std::memory_order_relaxed);
    capture.reset();
    referenceRequest.store (0, std::memory_order_relaxed);
    referenceCaptureId.store (0, std::memory_order_relaxed);
    referenceCapturedAtMilliseconds.store (0, std::memory_order_relaxed);
    referenceCompatibility.store (
        spectrumviewer::SpectrumReferenceCompatibility::noReference,
        std::memory_order_relaxed);
    spectrumReference.reset();
    analysisReadiness.store (SpectrumAnalysisReadiness::stopped, std::memory_order_release);
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
        && (capture.state.load (std::memory_order_acquire) == SpectrumCaptureState::live
            || capture.state.load (std::memory_order_acquire) == SpectrumCaptureState::failed))
        requestAnalysisConfiguration();
}

bool SpectrumViewer::startSpectrumCapture (double durationSeconds)
{
    const auto fineWindowSeconds = getProfileSettings (
        SpectrumAnalysisProfile::fine).windowSeconds;
    if (! acquisitionRunning.load (std::memory_order_acquire)
        || ! std::isfinite (durationSeconds) || durationSeconds <= 0.0)
        return false;

    const auto target = std::ceil (durationSeconds / fineWindowSeconds);
    if (target < 1.0
        || target > static_cast<double> (std::numeric_limits<std::size_t>::max()))
        return false;

    capture.requestedId.store (nextCaptureId++, std::memory_order_release);
    capture.targetWindows.store (static_cast<std::size_t> (target),
                                 std::memory_order_release);
    capture.resetProgress();
    capture.state.store (SpectrumCaptureState::preparing, std::memory_order_release);
    requestAnalysisConfiguration (true);
    return true;
}

bool SpectrumViewer::setCurrentCaptureAsReference() noexcept
{
    if (capture.state.load (std::memory_order_acquire) != SpectrumCaptureState::frozen)
        return false;
    // Name the capture the worker actually holds, not the one that was last
    // requested: a requested id with no CapturedSpectrum behind it would be
    // consumed by applyReferenceRequest() and dropped.
    const auto captureId = capture.retainedId.load (std::memory_order_acquire);
    if (captureId == 0)
        return false;
    // Captures use the Fine estimator. Keep the restored live analysis
    // compatible so a reference comparison works without a hidden profile
    // prerequisite.
    setAnalysisProfile (SpectrumAnalysisProfile::fine);
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
    const auto state = capture.state.load (std::memory_order_acquire);
    if (state == SpectrumCaptureState::live
        || state == SpectrumCaptureState::restoringLive)
        return;

    if (! acquisitionRunning.load (std::memory_order_acquire))
    {
        capture.state.store (SpectrumCaptureState::live, std::memory_order_release);
        return;
    }

    capture.replacementFailureState.store (SpectrumCaptureState::frozen,
                                           std::memory_order_relaxed);
    capture.state.store (SpectrumCaptureState::restoringLive,
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
        diagnostics.configurationFailures.fetch_add (1, std::memory_order_relaxed);
        if (forCapture)
            capture.state.store (SpectrumCaptureState::failed,
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
        request.captureId = capture.requestedId.load (std::memory_order_acquire);
        request.captureTargetWindowCount = capture.targetWindows.load (
            std::memory_order_acquire);
    }
    request.sourceChannelIndices.assign (
        acquisitionChannels.begin(),
        acquisitionChannels.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));
    request.sourceChannelUnits.assign (
        acquisitionChannelUnits.begin(),
        acquisitionChannelUnits.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));
    request.sourceStreamId = acquisitionStream;
    request.sourceGlobalChannelIndices.assign (
        acquisitionGlobalChannels.begin(),
        acquisitionGlobalChannels.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));

    requestedConfigurationGeneration.store (request.parameters.generation,
                                            std::memory_order_release);
    configurationPending.store (true, std::memory_order_release);
    if (! hasActiveAnalysis())
        analysisReadiness.store (SpectrumAnalysisReadiness::preparing, std::memory_order_release);
    else
        analysisReadiness.store (SpectrumAnalysisReadiness::live, std::memory_order_release);
    asynchronousAnalysis.request (std::move (request));
}
