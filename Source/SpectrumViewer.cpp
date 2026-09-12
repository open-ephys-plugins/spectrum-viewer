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
    else
    {
        // The named capture is not retained: finalizeCapturedSpectrum() could
        // not allocate it, or the runtime holding it was released first. The
        // exchange above has already consumed the request, so without this
        // counter the UI would keep showing the reference controls it enabled
        // on the click and never learn that nothing was set.
        droppedReferenceRequests.fetch_add (1, std::memory_order_release);
        LOGE ("Spectrum Viewer could not set capture ", request,
              " as the reference because it was not retained");
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
            quality,
            frameFifo.getSourceStreamId());
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
                                     capture,
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
        // publishReducedSpectrum only succeeds on a stable snapshot, so this
        // read cannot legitimately fail. If it does, leave the watermark alone
        // so the frozen capture is republished on the next pass rather than
        // recorded against a sequence that was never used.
        DisplaySettings settings;
        if (tryReadDisplaySettings (settings))
        {
            captureLastPublishedDisplaySettings = settings.sequence;
            captureLastPublishedComparisonSettings = comparisonSettingsSequence.load (
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
        invalidMappedInputBlocks.fetch_add (1, std::memory_order_relaxed);
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
        unconfiguredInputBlocks.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    const auto incomingSampleCount = static_cast<std::size_t> (
        getNumSamplesInBlock (route.streamId));
    if (incomingSampleCount == 0)
        return;

    if (route.generation == 0)
    {
        unconfiguredInputBlocks.fetch_add (1, std::memory_order_relaxed);
        unconfiguredInputSamples.fetch_add (incomingSampleCount, std::memory_order_relaxed);
        return;
    }

    std::array<const float*, MAX_CHANS> channelData {};

    for (std::size_t channel = 0; channel < route.channelCount; ++channel)
    {
        const auto globalChannel = route.globalChannelIndices[channel];
        if (globalChannel < 0 || globalChannel >= continuousBuffer.getNumChannels())
        {
            invalidMappedInputBlocks.fetch_add (1, std::memory_order_relaxed);
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
        droppedInputBlocks.store (fifo->getDroppedBlockCount(), std::memory_order_relaxed);
        droppedInputSamples.store (fifo->getDroppedSampleCount(), std::memory_order_relaxed);
        rejectedInputBlocks.store (fifo->getRejectedBlockCount(), std::memory_order_relaxed);
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
            captureState.store (replacementFailureCaptureState.load (
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

    const auto state = captureState.load (std::memory_order_acquire);
    if (state != SpectrumCaptureState::live
        && state != SpectrumCaptureState::failed)
    {
        replacementFailureCaptureState.store (
            state == SpectrumCaptureState::capturing
                ? SpectrumCaptureState::capturing
                : state == SpectrumCaptureState::frozen
                      ? SpectrumCaptureState::frozen
                      : SpectrumCaptureState::live,
            std::memory_order_relaxed);
        captureState.store (SpectrumCaptureState::restoringLive,
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
    completedCapture.reset();
    captureHasFirstSample = false;
    captureCompletionPending = false;
    captureFirstSample = 0;
    captureLastSampleExclusive = 0;
    captureLastFrameSequence = 0;
    captureTargetWindows.store (0, std::memory_order_relaxed);
    captureIncludedWindows.store (0, std::memory_order_relaxed);
    captureWindowSeconds.store (0.0, std::memory_order_relaxed);
    captureWallSpanSeconds.store (0.0, std::memory_order_relaxed);
    captureFailedWindows.store (0, std::memory_order_relaxed);
    captureShedWindows.store (0, std::memory_order_relaxed);
    captureDiscontinuities.store (0, std::memory_order_relaxed);
    captureState.store (SpectrumCaptureState::live, std::memory_order_release);

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
        droppedInputBlocks.store (0, std::memory_order_relaxed);
        droppedInputSamples.store (0, std::memory_order_relaxed);
        rejectedInputBlocks.store (0, std::memory_order_relaxed);
        invalidMappedInputBlocks.store (0, std::memory_order_relaxed);
        inputDiscontinuities.store (0, std::memory_order_relaxed);
        failedSpectrumWindows.store (0, std::memory_order_relaxed);
        shedSpectrumWindows.store (0, std::memory_order_relaxed);
        unconfiguredInputBlocks.store (0, std::memory_order_relaxed);
        unconfiguredInputSamples.store (0, std::memory_order_relaxed);
        staleConfigurationBlocks.store (0, std::memory_order_relaxed);
        configurationFailures.store (0, std::memory_order_relaxed);
        inputRouteInvalidated.store (false, std::memory_order_relaxed);
        droppedReferenceRequests.store (0, std::memory_order_relaxed);
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
    const auto state = captureState.load (std::memory_order_acquire);
    if (state == SpectrumCaptureState::live
        || state == SpectrumCaptureState::restoringLive)
        return;

    if (! acquisitionRunning.load (std::memory_order_acquire))
    {
        captureState.store (SpectrumCaptureState::live, std::memory_order_release);
        return;
    }

    replacementFailureCaptureState.store (SpectrumCaptureState::frozen,
                                           std::memory_order_relaxed);
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
