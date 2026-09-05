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

#ifndef SPECTRUM_VIEWER_H_INCLUDED
#define SPECTRUM_VIEWER_H_INCLUDED

#include <ProcessorHeaders.h>

#include "AtomicSynchronizer.h"
#include "CumulativeTFR.h"

#include <array>
#include <atomic>
#include <vector>

static constexpr int MAX_SPECTRUM_CHANNELS = 16;

enum DisplayType
{
    POWER_SPECTRUM = 1,
    SPECTROGRAM = 2
};

class SpectrumViewer;

/*

	Compute and display power spectra for incoming
	continuous channels.

*/
class SpectrumViewer : public GenericProcessor,
                       public Thread
{
public:
    /** Constructor */
    SpectrumViewer();

    /** Stops the FFT worker before its processing state is destroyed. */
    ~SpectrumViewer() override;

    /** Register parameters for this processor */
    void registerParameters() override;

    /** Create SpectrumViewerEditor */
    AudioProcessorEditor* createEditor() override;

    /** Called when upstream settings are updated */
    void updateSettings() override;

    /** Update buffers for FFT calculation*/
    void process (AudioBuffer<float>& continuousBuffer) override;

    /** Launch FFT calculation thread */
    bool startAcquisition() override;

    /** Stop FFT calculation thread*/
    bool stopAcquisition() override;

    /** Run FFT calculation in a separate thread */
    void run() override;

    /** Called when parameter value is updated*/
    void parameterValueChanged (Parameter* param) override;

    /** Returns a snapshot of the selected local channel indices. */
    Array<int> getActiveChans() const;

    /** Returns the callback-safe number of channels currently being processed. */
    int getNumActiveChannels() const noexcept { return activeChannelCount.load (std::memory_order_acquire); }

    /** Returns the name of the selected channel at a given index */
    String getChanName (int localIdx);

    /** Updates the requested frequency range and rebuilds worker-owned state. */
    void setFrequencyRange (Range<int> newRange);

    /** Returns the effective FFT-bin spacing in Hz. */
    float getFreqStep() const noexcept { return tfrParams.frequencyStep; }

    /** Returns the effective range after clamping to the stream's Nyquist frequency. */
    Range<int> getFrequencyRange() const noexcept { return { tfrParams.frequencyStart, tfrParams.frequencyEnd }; }

    /** Holds incoming samples and outgoing powers */
    struct PowerBuffer
    {
        /** Lock-free audio-thread to FFT-thread sample queue */
        std::vector<float> sampleQueue;

        AbstractFifo sampleFifo { 1 };
        std::atomic<size_t> droppedSampleCount { 0 };

        /** Latest outgoing power spectrum */
        AtomicallyShared<std::vector<float>> power;

        /** FFT-thread-owned rolling sample history */
        std::vector<float> sampleHistory;

        /** Size of each buffer in samples */
        int bufferSize = 0;

        /** Step size in samples */
        int stepSize = 0;

        /** Number of fft frequencies */
        int nFreqs = 0;

        int historyWriteIndex = 0;
        int validHistorySamples = 0;
        int samplesSinceLastFrame = 0;

        /** Copies incoming samples into the SPSC queue without waiting. */
        bool enqueue (const float* samples, int numSamples) noexcept;

        /** Drains queued samples and computes complete FFT frames. */
        bool processPendingSamples (CumulativeTFR& tfr,
                        FFTWArrayType& fftBuffer,
                        const std::vector<float>& window,
                        int channelIndex);

        /** Allocates all storage while acquisition processing is gated. */
        void configure (int bufferSize_, int stepSize_, int nFreqs_);

        /** Resets queue and processing indices while no producer or worker is active. */
        void reset()
        {
            sampleFifo.reset();
            droppedSampleCount.store (0, std::memory_order_relaxed);
            historyWriteIndex = 0;
            validHistorySamples = 0;
            samplesSinceLastFrame = 0;

            // The canvas holds read access only for the duration of one refresh.
            // Wait here, off the audio thread, rather than resizing an in-use slot.
            while (! power.reset())
                Thread::yield();
        }
    };

    /** Preallocated channel buffers shared with the canvas. */
    PowerBuffer powerBuffers[MAX_SPECTRUM_CHANNELS];

private:
    ScopedPointer<CumulativeTFR> TFR;
    FFTWArrayType fftBuffer;
    std::vector<float> fftWindow;

    static constexpr Thread::Priority THREAD_PRIORITY = Thread::Priority::normal;

    /** Resets buffers*/
    void resetTFR();

    /** Rebuilds processing storage while keeping the audio callback wait-free. */
    void reconfigureProcessing();

    /** Publishes a callback-safe snapshot of the selected channels. */
    void updateChannelSnapshot();

    Array<int> channels;
    std::array<std::atomic<int>, MAX_SPECTRUM_CHANNELS> activeGlobalChannelIndices;
    std::atomic<int> activeChannelCount { 0 };
    std::atomic<bool> reconfigurationInProgress { false };
    std::atomic<int> activeAudioCallbacks { 0 };

    uint16 activeStream = 0;

    struct TFRParameters
    {
        float windowLengthSeconds = 0.25f;
        float stepLengthSeconds = 0.020f;
        int numFrequencies = 250;
        float frequencyStep = 4.0f;
        int frequencyStart = 0;
        int frequencyEnd = 1000;
        float sampleRate = 2000.0f;
    };

    TFRParameters tfrParams;
    Range<int> requestedFrequencyRange { 0, 1000 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumViewer);
};

#endif // SPECTRUM_VIEWER_H_INCLUDED
