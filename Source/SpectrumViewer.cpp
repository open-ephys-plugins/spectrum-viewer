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
            return { 2.0, 0.5, 3.0, 5 };
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
    if (newRange.getEnd() != tfrParams.freqEnd)
    {
        tfrParams.freqEnd = newRange.getEnd();

        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        if (auto* currentEditor = getEditor())
            currentEditor->updateVisualizer();
    }
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
        bool consumedBlock = false;
        auto* fifo = inputFifo.get();
        auto* pipeline = activeAnalysis != nullptr ? &activeAnalysis->getPipeline() : nullptr;

        if (fifo != nullptr && pipeline != nullptr)
        {
            while (! threadShouldExit())
            {
                adoptPreparedAnalysis();
                pipeline = activeAnalysis != nullptr ? &activeAnalysis->getPipeline() : nullptr;
                if (pipeline == nullptr)
                    break;

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
                };
                const auto popped = fifo->tryPop (appendBlock);

                if (! popped)
                    break;

                consumedBlock = true;
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

                auto* outputFifo = &activeAnalysis->getFrameFifo();
                const auto publishFrame = [outputFifo] (const auto& frame)
                {
                    if (outputFifo != nullptr)
                        outputFifo->tryPush (frame.getChannelData (0),
                                             frame.numChannels,
                                             frame.numBins,
                                             frame.firstSample,
                                             frame.sequence);
                };
                pipeline->consumeReadyFrames (publishFrame);
                failedSpectrumWindows.store (pipeline->getFailedWindowCount(), std::memory_order_relaxed);

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
            acquisitionChannels[channel] = channels[static_cast<int> (channel)];

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
        unconfiguredInputBlocks.store (0, std::memory_order_relaxed);
        unconfiguredInputSamples.store (0, std::memory_order_relaxed);
        staleConfigurationBlocks.store (0, std::memory_order_relaxed);
        configurationFailures.store (0, std::memory_order_relaxed);
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

    if (acquisitionRunning.load (std::memory_order_acquire))
        requestAnalysisConfiguration();
}

void SpectrumViewer::requestAnalysisConfiguration()
{
    if (! acquisitionRunning.load (std::memory_order_acquire))
        return;

    const auto settings = getProfileSettings (analysisProfile.load (std::memory_order_relaxed));
    const auto windowSamples = std::round (acquisitionSampleRateHz * settings.windowSeconds);
    const auto hopSamples = std::round (acquisitionSampleRateHz * settings.hopSeconds);

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
    request.sourceChannelIndices.assign (
        acquisitionChannels.begin(),
        acquisitionChannels.begin() + static_cast<std::ptrdiff_t> (acquisitionChannelCount));

    requestedConfigurationGeneration.store (request.parameters.generation,
                                             std::memory_order_release);
    configurationPending.store (true, std::memory_order_release);
    if (! hasActiveAnalysis())
        analysisReadiness.store (SpectrumAnalysisReadiness::preparing, std::memory_order_release);
    else
        analysisReadiness.store (SpectrumAnalysisReadiness::live, std::memory_order_release);
    asynchronousAnalysis.request (std::move (request));
}
