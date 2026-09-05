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

#include <algorithm>

SpectrumViewer::SpectrumViewer()
    : GenericProcessor ("Spectrum Viewer"), Thread ("FFT Thread")
{
}

SpectrumViewer::~SpectrumViewer()
{
    // Close the callback gate before destroying any state the audio thread can access.
    reconfigurationInProgress.store (true, std::memory_order_release);

    while (activeAudioCallbacks.load (std::memory_order_acquire) != 0)
        Thread::yield();

    stopThread (-1);
}

bool SpectrumViewer::PowerBuffer::enqueue (const float* samples, int numSamples) noexcept
{
    if (sampleQueue.empty() || numSamples <= 0)
        return false;

    // The scoped region publishes its block sizes to the reader on destruction.
    const auto writeScope = sampleFifo.write (numSamples);
    const int samplesToWrite = writeScope.blockSize1 + writeScope.blockSize2;

    // Never block the audio thread. A full queue drops only the newest samples;
    // the worker reports the accumulated count at most once per second.
    droppedSampleCount.fetch_add ((size_t) (numSamples - samplesToWrite), std::memory_order_relaxed);

    if (samplesToWrite == 0)
        return false;

    if (writeScope.blockSize1 > 0)
        FloatVectorOperations::copy (sampleQueue.data() + writeScope.startIndex1, samples, writeScope.blockSize1);

    if (writeScope.blockSize2 > 0)
        FloatVectorOperations::copy (sampleQueue.data() + writeScope.startIndex2,
                                     samples + writeScope.blockSize1,
                                     writeScope.blockSize2);

    return true;
}

bool SpectrumViewer::PowerBuffer::processPendingSamples (CumulativeTFR& tfr,
                                                         FFTWArrayType& fftBuffer,
                                                         const std::vector<float>& window,
                                                         int channelIndex)
{
    if (bufferSize == 0)
        return false;

    // Read only as far as the next frame boundary. This limits worker latency
    // and gives every selected channel a chance to produce a frame each pass.
    const int samplesUntilFrame = jmax (bufferSize - validHistorySamples,
                                        stepSize - samplesSinceLastFrame);
    // The scoped region releases the consumed samples on destruction.
    const auto readScope = sampleFifo.read (jmax (1, samplesUntilFrame));
    const int samplesToRead = readScope.blockSize1 + readScope.blockSize2;

    if (samplesToRead == 0)
        return false;

    const auto copyToHistory = [this] (const float* source, int numSamples)
    {
        if (numSamples == 0)
            return;

        const int firstBlockSize = jmin (numSamples, bufferSize - historyWriteIndex);
        if (firstBlockSize > 0)
            FloatVectorOperations::copy (sampleHistory.data() + historyWriteIndex, source, firstBlockSize);

        const int secondBlockSize = numSamples - firstBlockSize;
        if (secondBlockSize > 0)
            FloatVectorOperations::copy (sampleHistory.data(), source + firstBlockSize, secondBlockSize);

        historyWriteIndex = (historyWriteIndex + numSamples) % bufferSize;
    };

    copyToHistory (sampleQueue.data() + readScope.startIndex1, readScope.blockSize1);
    copyToHistory (sampleQueue.data() + readScope.startIndex2, readScope.blockSize2);

    validHistorySamples = jmin (bufferSize, validHistorySamples + samplesToRead);
    samplesSinceLastFrame += samplesToRead;

    if (validHistorySamples == bufferSize && samplesSinceLastFrame >= stepSize)
    {
        // historyWriteIndex points to the oldest sample after the circular write.
        // Copy from there to present the FFT with a chronological, windowed frame.
        for (int index = 0; index < bufferSize; ++index)
        {
            const auto historyIndex = (historyWriteIndex + index) % bufferSize;
            fftBuffer.set (index, sampleHistory[(size_t) historyIndex] * window[(size_t) index]);
        }

        tfr.computeFFT (fftBuffer, channelIndex);

        // Publish only the latest complete spectrum. The triple buffer prevents
        // the worker from waiting for the canvas to finish its current refresh.
        AtomicScopedWritePtr<std::vector<float>> powerWriter (power);
        if (powerWriter.isValid())
        {
            tfr.getPower (powerWriter.operator*(), channelIndex);
            powerWriter.pushUpdate();
        }

        samplesSinceLastFrame = 0;
    }

    return true;
}

void SpectrumViewer::PowerBuffer::configure (int bufferSize_, int stepSize_, int nFreqs_)
{
    bufferSize = jmax (1, bufferSize_);
    stepSize = jmax (1, stepSize_);
    nFreqs = jmax (1, nFreqs_);

    // Keep at least two analysis windows and several update intervals buffered.
    // AbstractFifo reserves one slot to distinguish a full queue from an empty one.
    const int queueCapacity = jmax (4096, bufferSize * 2, stepSize * 16);
    sampleQueue.assign ((size_t) queueCapacity + 1, 0.0f);
    sampleFifo.setTotalSize ((int) sampleQueue.size());
    sampleHistory.assign ((size_t) bufferSize, 0.0f);
    // A canvas refresh may briefly own one read slot. Reconfiguration is not
    // real-time work, so wait for it instead of publishing mismatched sizes.
    while (! power.map ([this] (std::vector<float>& values) { values.assign ((size_t) nFreqs, 0.0f); }))
        Thread::yield();

    reset();
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
                                  MAX_SPECTRUM_CHANNELS,
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

        auto* stream = getDataStream (streamKey);
        if (stream == nullptr)
        {
            LOGD ("Spectrum Viewer ignored unavailable stream: ", streamKey);
            return;
        }

        // Publish the gate before changing activeStream; callbacks that are already
        // running are allowed to finish against the old configuration.
        reconfigurationInProgress.store (true, std::memory_order_release);
        while (activeAudioCallbacks.load (std::memory_order_acquire) != 0)
            Thread::yield();

        activeStream = stream->getStreamId();
        tfrParams.sampleRate = stream->getSampleRate();

        channels.clear();
        auto* p = static_cast<SelectedChannelsParameter*> (stream->getParameter ("Channels"));
        if (p != nullptr)
            channels = p->getArrayValue();

        reconfigureProcessing();
        getEditor()->updateVisualizer();
    }
    else if (param->getName() == "Channels")
    {
        channels.clear();

        auto* p = static_cast<SelectedChannelsParameter*> (param);

        channels = p->getArrayValue();

        reconfigureProcessing();

        getEditor()->updateVisualizer();
    }
}

void SpectrumViewer::setFrequencyRange (Range<int> newRange)
{
    if (newRange != requestedFrequencyRange)
    {
        requestedFrequencyRange = newRange;

        // Lower ranges use longer windows for finer frequency resolution.
        if (newRange.getEnd() == 100)
            tfrParams.windowLengthSeconds = 2.0f;
        else if (newRange.getEnd() == 500)
            tfrParams.windowLengthSeconds = 0.5f;
        else if (newRange.getEnd() == 1000)
            tfrParams.windowLengthSeconds = 0.25f;
        else
            tfrParams.windowLengthSeconds = 0.1f;

        reconfigureProcessing();

        getEditor()->updateVisualizer();
    }
}

void SpectrumViewer::process (AudioBuffer<float>& continuousBuffer)
{
    if (reconfigurationInProgress.load (std::memory_order_acquire))
        return;

    // Increment then recheck the gate to close the race where reconfiguration
    // starts between the first test and entering the callback's critical region.
    activeAudioCallbacks.fetch_add (1, std::memory_order_acq_rel);
    if (reconfigurationInProgress.load (std::memory_order_acquire))
    {
        activeAudioCallbacks.fetch_sub (1, std::memory_order_release);
        return;
    }

    const int numChannels = activeChannelCount.load (std::memory_order_acquire);
    const int incomingSampleCount = getNumSamplesInBlock (activeStream);
    bool queuedSamples = false;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        const int globalChannelIndex = activeGlobalChannelIndices[(size_t) channel].load (std::memory_order_relaxed);

        if (globalChannelIndex < 0 || globalChannelIndex >= continuousBuffer.getNumChannels())
            continue;

        // Channel mapping and queue storage are prepared off-thread, so this is
        // the only data movement required on the acquisition thread.
        const float* incomingData = continuousBuffer.getReadPointer (globalChannelIndex);
        queuedSamples |= powerBuffers[(size_t) channel].enqueue (incomingData, incomingSampleCount);
    }

    activeAudioCallbacks.fetch_sub (1, std::memory_order_release);

    if (queuedSamples)
        notify();
}

void SpectrumViewer::run()
{
    size_t droppedSamplesSinceLastLog = 0;
    double lastOverflowLogTime = Time::getMillisecondCounterHiRes();

    while (! threadShouldExit())
    {
        bool processedSamples = false;
        const int numChannels = activeChannelCount.load (std::memory_order_acquire);
        auto* tfr = TFR.get();

        if (tfr == nullptr)
        {
            wait (10);
            continue;
        }

        // Each call consumes at most one frame per channel, preventing a channel
        // with a large backlog from starving the remaining channels.
        for (int channel = 0; channel < numChannels && ! threadShouldExit(); ++channel)
        {
            processedSamples |= powerBuffers[(size_t) channel].processPendingSamples (*tfr, fftBuffer, fftWindow, channel);
            droppedSamplesSinceLastLog += powerBuffers[(size_t) channel].droppedSampleCount.exchange (0, std::memory_order_relaxed);
        }

        const double currentTime = Time::getMillisecondCounterHiRes();
        if (droppedSamplesSinceLastLog > 0 && currentTime - lastOverflowLogTime >= 1000.0)
        {
            LOGD ("Spectrum Viewer dropped ", droppedSamplesSinceLastLog, " queued samples while the FFT worker was overloaded");
            droppedSamplesSinceLastLog = 0;
            lastOverflowLogTime = currentTime;
        }

        // Avoid polling continuously while preserving a bounded wake-up latency
        // if a notification happens just before this wait.
        if (! processedSamples)
            wait (10);
    }
}

void SpectrumViewer::updateSettings()
{
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
    reconfigureProcessing();
}

void SpectrumViewer::resetTFR()
{
    TFR.reset (new CumulativeTFR (jmax (1, activeChannelCount.load (std::memory_order_acquire)),
                                  tfrParams.numFrequencies,
                                  tfrParams.frequencyStep,
                                  tfrParams.frequencyStart));
}

void SpectrumViewer::updateChannelSnapshot()
{
    const int channelCount = jmin (channels.size(), MAX_SPECTRUM_CHANNELS);

    // Resolve local-to-global indices once here instead of querying the graph
    // for every channel in every audio callback.
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const int globalChannelIndex = getGlobalChannelIndex (activeStream, channels[channel]);
        activeGlobalChannelIndices[(size_t) channel].store (globalChannelIndex, std::memory_order_relaxed);
    }

    activeChannelCount.store (channelCount, std::memory_order_release);
}

void SpectrumViewer::reconfigureProcessing()
{
    // The audio callback observes this flag and returns immediately; it never
    // waits for allocations, FFT plan creation, or worker shutdown.
    reconfigurationInProgress.store (true, std::memory_order_release);

    while (activeAudioCallbacks.load (std::memory_order_acquire) != 0)
        Thread::yield();

    // All worker-owned objects below can be replaced only after run() exits.
    const bool restartWorker = isThreadRunning();
    if (restartWorker)
        stopThread (-1);

    const int bufferSize = jmax (2, roundToInt (tfrParams.sampleRate * tfrParams.windowLengthSeconds));
    const int stepSize = jmax (1, roundToInt (tfrParams.stepLengthSeconds * tfrParams.sampleRate));
    tfrParams.frequencyStep = tfrParams.sampleRate / (float) bufferSize;
    const int nyquistFrequency = jmax (1, (int) (tfrParams.sampleRate * 0.5f));
    tfrParams.frequencyStart = jlimit (0, nyquistFrequency - 1, requestedFrequencyRange.getStart());
    tfrParams.frequencyEnd = jlimit (tfrParams.frequencyStart + 1,
                                     nyquistFrequency,
                                     requestedFrequencyRange.getEnd());

    // Real FFT output contains bins [0, N/2]. Clamp the requested range to
    // that interval before sizing output vectors or indexing FFT data.
    const int firstBin = roundToInt (tfrParams.frequencyStart / tfrParams.frequencyStep);
    const int availableBins = jmax (1, bufferSize / 2 + 1 - firstBin);
    const int requestedBins = jmax (1, (int) ((tfrParams.frequencyEnd - tfrParams.frequencyStart) / tfrParams.frequencyStep));
    tfrParams.numFrequencies = jmin (requestedBins, availableBins);

    // The worker processes channels serially, so one transform buffer and one
    // window are shared by all channels instead of duplicated per channel.
    fftBuffer.resize (bufferSize);
    fftWindow.resize ((size_t) bufferSize);
    const float windowDenominator = (float) (bufferSize - 1);
    for (int sample = 0; sample < bufferSize; ++sample)
        fftWindow[(size_t) sample] = 0.54f - 0.46f * std::cos (MathConstants<float>::twoPi * sample / windowDenominator);

    updateChannelSnapshot();
    const int channelCount = activeChannelCount.load (std::memory_order_acquire);

    for (int channel = 0; channel < channelCount; ++channel)
        powerBuffers[(size_t) channel].configure (bufferSize, stepSize, tfrParams.numFrequencies);

    resetTFR();

    if (restartWorker && ! startThread (THREAD_PRIORITY))
        LOGD ("Spectrum Viewer failed to restart its FFT worker thread after reconfiguration");

    reconfigurationInProgress.store (false, std::memory_order_release);
}

Array<int> SpectrumViewer::getActiveChans() const
{
    return channels;
}

String SpectrumViewer::getChanName (int localIdx)
{
    auto* stream = getDataStream (activeStream);
    if (stream == nullptr || ! isPositiveAndBelow (localIdx, stream->getContinuousChannels().size()))
        return "Channel " + String (localIdx + 1);

    return stream->getContinuousChannels()[localIdx]->getName();
}

bool SpectrumViewer::startAcquisition()
{
    if (! isEnabled)
        return false;

    // Normally parameters configure the worker first; this also supports hosts
    // that start acquisition before delivering an initial parameter callback.
    if (TFR == nullptr)
        reconfigureProcessing();

    const int channelCount = activeChannelCount.load (std::memory_order_acquire);
    for (int channel = 0; channel < channelCount; ++channel)
        powerBuffers[(size_t) channel].reset();

    const bool workerStarted = startThread (THREAD_PRIORITY);
    if (! workerStarted)
        LOGD ("Spectrum Viewer failed to start its FFT worker thread");

    return workerStarted;
}

bool SpectrumViewer::stopAcquisition()
{
    stopThread (-1);
    return true;
}