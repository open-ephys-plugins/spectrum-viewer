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

#define MS_FROM_START Time::highResolutionTicksToSeconds (Time::getHighResolutionTicks() - start) * 1000

SpectrumViewer::SpectrumViewer()
    : GenericProcessor ("Spectrum Viewer"), Thread ("FFT Thread"), displayType (POWER_SPECTRUM)
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
    stopThread (1000);
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
            getEditor()->updateVisualizer();
        }
    }
    else if (param->getName() == "Channels")
    {
        channels.clear();

        SelectedChannelsParameter* p = (SelectedChannelsParameter*) param;

        channels = p->getArrayValue();

        getEditor()->updateVisualizer();
    }
}

void SpectrumViewer::setFrequencyRange (Range<int> newRange)
{
    if (newRange.getEnd() != tfrParams.freqEnd)
    {
        tfrParams.freqEnd = newRange.getEnd();

        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        getEditor()->updateVisualizer();
    }
}

void SpectrumViewer::process (AudioBuffer<float>& continuousBuffer)
{
    auto* fifo = inputFifo.get();
    if (fifo == nullptr || acquisitionChannelCount == 0)
        return;

    const auto incomingSampleCount = static_cast<std::size_t> (getNumSamplesInBlock (activeStream));
    if (incomingSampleCount == 0)
        return;

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
                         acquisitionGeneration))
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
        bool consumedBlock = false;
        auto* fifo = inputFifo.get();
        auto* pipeline = analysisPipeline.get();

        if (fifo != nullptr && pipeline != nullptr)
        {
            while (! threadShouldExit())
            {
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
                    jassertfalse;
                    LOGE ("Spectrum Viewer worker rejected a FIFO block");
                    pipeline->reset();
                    continue;
                }

                if (appendResult.discontinuity)
                    inputDiscontinuities.store (pipeline->getDiscontinuityCount(), std::memory_order_relaxed);

                auto* outputFifo = spectrumFrameFifo.get();
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
            }
        }

        // Thread::notify() takes a mutex in JUCE 8, so the audio callback must not
        // use it. A short worker-only timed wait avoids both callback locks and spin.
        if (! consumedBlock)
            wait (2.0);
    }
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
        const auto windowDuration = static_cast<double> (tfrParams.winLen);
        const auto hopDuration = static_cast<double> (tfrParams.stepLen);
        if (acquisitionChannelCount == 0 || maximumInputBlockSamples <= 0
            || ! std::isfinite (sampleRate) || sampleRate <= 0.0
            || ! std::isfinite (windowDuration) || windowDuration <= 0.0
            || ! std::isfinite (hopDuration) || hopDuration <= 0.0)
            return isEnabled;
        const auto windowSamples = std::round (sampleRate * windowDuration);
        const auto hopSamples = std::round (sampleRate * hopDuration);
        if (! std::isfinite (windowSamples) || windowSamples < 1.0
            || windowSamples >= static_cast<double> (std::numeric_limits<std::size_t>::max())
            || ! std::isfinite (hopSamples) || hopSamples < 1.0
            || hopSamples >= static_cast<double> (std::numeric_limits<std::size_t>::max()))
            return isEnabled;
        const auto windowSampleCount = static_cast<std::size_t> (windowSamples);
        const auto hopSampleCount = static_cast<std::size_t> (hopSamples);

        for (std::size_t channel = 0; channel < acquisitionChannelCount; ++channel)
            acquisitionChannels[channel] = channels[static_cast<int> (channel)];

        const auto nextGeneration = acquisitionGeneration + 1;
        spectrumviewer::SpectrumAnalysisParameters parameters;
        parameters.channelCount = acquisitionChannelCount;
        parameters.windowSampleCount = windowSampleCount;
        parameters.hopSampleCount = hopSampleCount;
        parameters.maximumInputBlockSampleCount = static_cast<std::size_t> (maximumInputBlockSamples);
        parameters.sampleRateHz = sampleRate;
        // This is an explicit integration profile, not a claim that NW=2/K=3
        // is the final diagnostic default. Profile controls follow in PR4/PR5.
        parameters.timeHalfBandwidth = 2.0;
        parameters.taperCount = 3;
        parameters.detrendMode = spectrumviewer::DetrendMode::mean;
        parameters.generation = nextGeneration;

        try
        {
            analysisConfiguration = std::make_shared<const spectrumviewer::SpectrumAnalysisConfiguration> (parameters);
            analysisPipeline = std::make_unique<spectrumviewer::SpectrumAnalysisPipeline> (analysisConfiguration);
            inputFifo = std::make_unique<spectrumviewer::SampleBlockFifo> (
                acquisitionChannelCount,
                static_cast<std::size_t> (maximumInputBlockSamples),
                INPUT_QUEUE_CAPACITY);
            spectrumFrameFifo = std::make_unique<spectrumviewer::SpectrumFrameFifo> (
                acquisitionChannelCount,
                analysisConfiguration->getBinCount(),
                OUTPUT_QUEUE_CAPACITY,
                analysisConfiguration->getFrameDescriptor(),
                std::vector<int> (acquisitionChannels.begin(),
                                  acquisitionChannels.begin()
                                      + static_cast<std::ptrdiff_t> (acquisitionChannelCount)));
        }
        catch (const std::exception& error)
        {
            LOGE ("Unable to configure Spectrum Viewer analysis: ", error.what());
            analysisPipeline.reset();
            analysisConfiguration.reset();
            inputFifo.reset();
            spectrumFrameFifo.reset();
            acquisitionChannelCount = 0;
            return false;
        }

        acquisitionGeneration = nextGeneration;
        droppedInputBlocks.store (0, std::memory_order_relaxed);
        droppedInputSamples.store (0, std::memory_order_relaxed);
        rejectedInputBlocks.store (0, std::memory_order_relaxed);
        inputDiscontinuities.store (0, std::memory_order_relaxed);
        failedSpectrumWindows.store (0, std::memory_order_relaxed);
        startThread (Thread::Priority::normal);
    }
    return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
    stopThread (1000);
    analysisPipeline.reset();
    analysisConfiguration.reset();
    inputFifo.reset();
    spectrumFrameFifo.reset();
    acquisitionChannelCount = 0;
    return true;
}
