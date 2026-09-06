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

#define MS_FROM_START Time::highResolutionTicksToSeconds (Time::getHighResolutionTicks() - start) * 1000

SpectrumViewer::SpectrumViewer()
    : GenericProcessor ("Spectrum Viewer"), Thread ("FFT Thread"), displayType (POWER_SPECTRUM)
{
    tfrParams.segLen = 1;
    tfrParams.freqStart = 0;
    tfrParams.freqEnd = 1000;
    tfrParams.stepLen = 0.020; // update every 20 ms (50 Hz)
    tfrParams.winLen = 0.25;
    tfrParams.interpRatio = 1;
    tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
    tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);
    tfrParams.Fs = 2000;
    tfrParams.alpha = 0;
    tfrParams.nTimes = 1;

    bufferResizer = std::make_unique<BufferResizer> (this);
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

        int bufferSize = int (tfrParams.Fs * tfrParams.winLen);

        for (int i = 0; i < MAX_CHANS; i++)
        {
            powerBuffers[i].setBufferSize (bufferSize, tfrParams.stepLen * tfrParams.Fs);
            powerBuffers[i].setNumFreqs (tfrParams.nFreqs);
        }

        bufferResizer->resize();

        resetTFR();

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

        if (tfrParams.freqEnd == 100)
            tfrParams.winLen = 2;
        else if (tfrParams.freqEnd == 500)
            tfrParams.winLen = 0.5;
        else if (tfrParams.freqEnd == 1000)
            tfrParams.winLen = 0.25;
        else
            tfrParams.winLen = 0.1;

        tfrParams.freqStep = 1.0 / float (tfrParams.winLen * tfrParams.interpRatio);
        tfrParams.nFreqs = int ((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

        int bufferSize = int (tfrParams.Fs * tfrParams.winLen);

        for (int i = 0; i < MAX_CHANS; i++)
        {
            powerBuffers[i].setBufferSize (bufferSize, tfrParams.stepLen * tfrParams.Fs);
            powerBuffers[i].setNumFreqs (tfrParams.nFreqs);
        }

        bufferResizer->resize();
        resetTFR();

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
        auto* assembler = windowAssembler.get();

        if (fifo != nullptr && assembler != nullptr)
        {
            while (! threadShouldExit() && fifo->tryPop ([&] (const auto& block)
            {
                consumedBlock = true;
                std::array<const float*, MAX_CHANS> channelData {};
                for (std::size_t channel = 0; channel < block.numChannels; ++channel)
                    channelData[channel] = block.getChannelData (channel);

                if (! workerHasConfiguration
                    || block.configurationGeneration != workerConfigurationGeneration)
                {
                    assembler->reset();
                    workerConfigurationGeneration = block.configurationGeneration;
                    workerHasConfiguration = true;
                }

                const auto result = assembler->append (channelData.data(),
                                                       block.numChannels,
                                                       block.numSamples,
                                                       block.firstSample,
                                                       [this] (const auto& window)
                {
                    processWindow (window);
                });

                if (result.discontinuity)
                    inputDiscontinuities.store (assembler->getDiscontinuityCount(), std::memory_order_relaxed);
            }))
            {
            }
        }

        // Thread::notify() takes a mutex in JUCE 8, so the audio callback must not
        // use it. A short worker-only timed wait avoids both callback locks and spin.
        if (! consumedBlock)
            wait (2.0);
    }
}

void SpectrumViewer::processWindow (const spectrumviewer::SampleWindowAssembler::WindowView& window)
{
    if (TFR == nullptr || window.numChannels != static_cast<std::size_t> (fftBuffers.size()))
        return;

    for (std::size_t channel = 0; channel < window.numChannels; ++channel)
    {
        auto& fftBuffer = *fftBuffers[static_cast<int> (channel)];
        const auto channelView = window.getChannel (channel);
        std::size_t destination = 0;

        const auto copyRegion = [&] (const float* source, std::size_t count)
        {
            for (std::size_t sample = 0; sample < count; ++sample, ++destination)
                fftBuffer.set (static_cast<int> (destination),
                               source[sample] * powerBuffers[channel].window[static_cast<int> (destination)]);
        };

        copyRegion (channelView.firstData, channelView.firstSize);
        copyRegion (channelView.secondData, channelView.secondSize);
        TFR->computeFFT (fftBuffer, static_cast<int> (channel));

        auto& outputs = powerBuffers[channel].power;
        if (outputs.isEmpty())
            continue;

        auto& output = *outputs[static_cast<int> (nextPowerSlot % static_cast<std::size_t> (outputs.size()))];
        AtomicScopedWritePtr<std::vector<float>> powerWriter (output);
        if (powerWriter.isValid())
        {
            TFR->getPower (powerWriter.operator*(), static_cast<int> (channel));
            powerWriter.pushUpdate();
        }
    }

    ++nextPowerSlot;
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

void SpectrumViewer::resetTFR()
{
    TFR.reset (new CumulativeTFR (MAX_CHANS, // channel count
                                  tfrParams.nFreqs,
                                  tfrParams.nTimes,
                                  tfrParams.Fs, // sample rate
                                  tfrParams.winLen,
                                  tfrParams.stepLen,
                                  tfrParams.freqStep,
                                  tfrParams.freqStart,
                                  tfrParams.segLen, //fftSec
                                  tfrParams.alpha));
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
        bufferResizer->waitForThreadToExit (5000);

        for (int i = 0; i < MAX_CHANS; i++)
        {
            powerBuffers[i].reset();
        }

        acquisitionChannelCount = static_cast<std::size_t> (std::min (channels.size(), MAX_CHANS));
        if (acquisitionChannelCount == 0 || powerBuffers[0].bufferSize <= 0 || powerBuffers[0].stepSize <= 0)
            return isEnabled;

        for (std::size_t channel = 0; channel < acquisitionChannelCount; ++channel)
            acquisitionChannels[channel] = channels[static_cast<int> (channel)];

        inputFifo = std::make_unique<spectrumviewer::SampleBlockFifo> (
            acquisitionChannelCount, MAX_INPUT_BLOCK_SAMPLES, INPUT_QUEUE_CAPACITY);
        windowAssembler = std::make_unique<spectrumviewer::SampleWindowAssembler> (
            acquisitionChannelCount,
            static_cast<std::size_t> (powerBuffers[0].bufferSize),
            static_cast<std::size_t> (powerBuffers[0].stepSize));

        fftBuffers.clear();
        for (std::size_t channel = 0; channel < acquisitionChannelCount; ++channel)
            fftBuffers.add (new FFTWArrayType (powerBuffers[0].bufferSize));

        nextPowerSlot = 0;
        ++acquisitionGeneration;
        workerConfigurationGeneration = 0;
        workerHasConfiguration = false;
        droppedInputBlocks.store (0, std::memory_order_relaxed);
        droppedInputSamples.store (0, std::memory_order_relaxed);
        rejectedInputBlocks.store (0, std::memory_order_relaxed);
        inputDiscontinuities.store (0, std::memory_order_relaxed);
        startThread (Thread::Priority::normal);
    }
    return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
    stopThread (1000);
    fftBuffers.clear();
    windowAssembler.reset();
    inputFifo.reset();
    acquisitionChannelCount = 0;
    return true;
}

BufferResizer::BufferResizer (SpectrumViewer* p)
    : Thread ("Spectrum Viewer buffer resizer"), processor (p)
{
    //setStatusMessage("Resizing buffers...");
}

void BufferResizer::resize()
{
    waitForThreadToExit (5000);

    run();
}

void BufferResizer::run()
{
    //setStatusMessage("Resizing data buffer for all channels");

    for (int i = 0; i < MAX_CHANS; i++)
    {
        processor->powerBuffers[i].resize();
    }
}
