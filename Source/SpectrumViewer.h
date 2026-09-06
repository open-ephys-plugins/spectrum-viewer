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

#include "CumulativeTFR.h"
#include "SampleBlockFifo.h"
#include "SampleWindowAssembler.h"
#include "SpectrumFrameFifo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <time.h>
#include <utility>
#include <vector>

#define MAX_CHANS 8

enum DisplayType
{
    POWER_SPECTRUM = 1,
    SPECTROGRAM = 2
};

class SpectrumViewer;

/*
	Resize analysis buffers and show a progress window
*/
class BufferResizer : public Thread
{
public:
    /** Constructor */
    BufferResizer (SpectrumViewer* processor);

    /** Resizes buffer */
    void resize();

private:
    /** Resizes buffer in the background */
    void run() override;

    /** Pointer to processor */
    SpectrumViewer* processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BufferResizer);
};

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

    /** Destructor */
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

    /** Called by the canvas to get the number of active chans*/
    Array<int> getActiveChans();

    /** Returns the name of the selected channel at a given index */
    const String getChanName (int localIdx);

    /** Sets the min/max frequency range*/
    void setFrequencyRange (Range<int>);

    /** Returns the frequency step for the currently selected range*/
    float getFreqStep() { return tfrParams.freqStep; };

    /** Returns whole input blocks dropped because the worker queue was full. */
    std::uint64_t getDroppedInputBlockCount() const noexcept { return droppedInputBlocks.load (std::memory_order_relaxed); }

    /** Returns input samples discarded as part of full-queue block drops. */
    std::uint64_t getDroppedInputSampleCount() const noexcept { return droppedInputSamples.load (std::memory_order_relaxed); }

    /** Returns input blocks rejected because their shape exceeded the fixed configuration. */
    std::uint64_t getRejectedInputBlockCount() const noexcept { return rejectedInputBlocks.load (std::memory_order_relaxed); }

    /** Returns sample-index gaps or overlaps observed by the worker. */
    std::uint64_t getInputDiscontinuityCount() const noexcept { return inputDiscontinuities.load (std::memory_order_relaxed); }

    /** Consumes all pending display frames and passes only the newest complete frame to consumer. */
    template <typename Consumer>
    bool consumeLatestSpectrumFrame (Consumer&& consumer)
    {
        auto* fifo = spectrumFrameFifo.get();
        return fifo != nullptr && fifo->tryPopLatest (std::forward<Consumer> (consumer));
    }

    std::uint64_t getDroppedSpectrumFrameCount() const noexcept
    {
        const auto* fifo = spectrumFrameFifo.get();
        return fifo != nullptr ? fifo->getDroppedFrameCount() : 0;
    }

    std::uint64_t getStaleSpectrumFrameCount() const noexcept
    {
        const auto* fifo = spectrumFrameFifo.get();
        return fifo != nullptr ? fifo->getStaleFrameCount() : 0;
    }

    /** Holds analysis dimensions and window coefficients. */
    struct PowerBuffer
    {
        /** Hamming window to apply to buffer */
        Array<float> window;

        /** Size of each buffer in samples */
        int bufferSize = 0;

        /** Step size in samples */
        int stepSize = 0;

        /** Steps per buffer samples */
        int stepsPerBuffer = 0;

        /** Number of fft frequencies */
        int nFreqs = 0;

        /** true if buffer size was updated */
        bool bufferSizeChanged = true;

        /** true if number of freqs was updated */
        bool numFreqsChanged = true;

        /** Changes buffer size*/
        void setBufferSize (int bufferSize_, int stepSize_)
        {
            const auto validStepSize = std::max (1, stepSize_);
            if (bufferSize != bufferSize_ || stepSize != validStepSize)
            {
                bufferSize = bufferSize_;
                stepSize = validStepSize;
                stepsPerBuffer = bufferSize / stepSize;
                bufferSizeChanged = true;
            }
        }

        /** Changes num freqs */
        void setNumFreqs (int nFreqs_)
        {
            if (nFreqs != nFreqs_)
            {
                nFreqs = nFreqs_;
                numFreqsChanged = true;
            }
        }

        /** Resizes all buffers */
        void resize()
        {
            if (bufferSizeChanged)
            {
                bufferSizeChanged = false;

                window.clear();

                const float N = float (bufferSize);
                const float PI = 3.1415926535;

                for (int n = 0; n < bufferSize; n++)
                {
                    window.add (0.54 - 0.46 * cos (2 * PI * n / N));
                }
            }

            numFreqsChanged = false;
        }
    };

    /** Array of buffers */
    PowerBuffer powerBuffers[MAX_CHANS];

    /** Type of visualization */
    DisplayType displayType;

private:
    /** Processes one complete chronological window on the analysis thread. */
    void processWindow (const spectrumviewer::SampleWindowAssembler::WindowView& window);

    ScopedPointer<CumulativeTFR> TFR;

    /** Resets buffers*/
    void resetTFR();

    Array<int> channels;

    static constexpr std::size_t INPUT_QUEUE_CAPACITY = 8;
    static constexpr std::size_t OUTPUT_QUEUE_CAPACITY = 8;
    static constexpr std::size_t MAX_INPUT_BLOCK_SAMPLES = 8192;
    std::unique_ptr<spectrumviewer::SampleBlockFifo> inputFifo;
    std::unique_ptr<spectrumviewer::SpectrumFrameFifo> spectrumFrameFifo;
    std::unique_ptr<spectrumviewer::SampleWindowAssembler> windowAssembler;
    OwnedArray<FFTWArrayType> fftBuffers;
    std::vector<float> powerScratch;
    std::array<int, MAX_CHANS> acquisitionChannels {};
    std::size_t acquisitionChannelCount = 0;
    std::uint64_t nextSpectrumFrameSequence = 0;
    std::uint64_t acquisitionGeneration = 0;
    std::uint64_t workerConfigurationGeneration = 0;
    bool workerHasConfiguration = false;
    std::atomic<std::uint64_t> droppedInputBlocks { 0 };
    std::atomic<std::uint64_t> droppedInputSamples { 0 };
    std::atomic<std::uint64_t> rejectedInputBlocks { 0 };
    std::atomic<std::uint64_t> inputDiscontinuities { 0 };

    //int bufferSize;
    //int stepSize;
    //int stepsPerBuffer;

    uint16 activeStream = 0;

    // This is to store data in case of switch and we wish to retrive old data
    //AtomicallyShared<Array<FFTWArrayType>> dataBufferII;
    //Array<AtomicallyShared<FFTWArrayType>> updatedDataBuffer;

    struct TFRParameters
    {
        float segLen; // Segment Length
        float winLen; // Window Length
        float stepLen; // Interval between times of interest
        int interpRatio; //

        // Number of freq of interest
        int nFreqs;
        float freqStep;
        int freqStart;
        int freqEnd;

        // Number of times of interest
        int nTimes;

        // Fs (sampling rate?)
        float Fs;

        // frequency of interest
        Array<float> foi;

        float alpha;
    };

    TFRParameters tfrParams;
    std::unique_ptr<BufferResizer> bufferResizer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumViewer);
};

#endif // SPECTRUM_VIEWER_H_INCLUDED
