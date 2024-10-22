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

#include <time.h>
#include <vector>
#include <chrono>
#include <ctime> 
#include <iostream>
#include <fstream>

#define MAX_CHANS 8

enum DisplayType {
	POWER_SPECTRUM = 1,
	SPECTROGRAM = 2
};

class SpectrumViewer;

/*
	Resize data and power buffers, and show a progress window
*/
class BufferResizer : public Thread
{
public:

	/** Constructor */
	BufferResizer(SpectrumViewer* processor);

	/** Resizes buffer */
	void resize();

private:

	/** Resizes buffer in the background */
	void run() override;

	/** Pointer to processor */
	SpectrumViewer* processor;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BufferResizer);
};

/*

	Compute and display power spectra for incoming
	continuous channels.

*/
class SpectrumViewer : 
	public GenericProcessor, 
	public Thread
{
public:

	/** Constructor */
	SpectrumViewer();

	/** Destructor */
    ~SpectrumViewer() { }

	/** Register parameters for this processor */
	void registerParameters() override;

	/** Create SpectrumViewerEditor */
	AudioProcessorEditor* createEditor() override;

	/** Called when upstream settings are updated */
	void updateSettings() override;

	/** Update buffers for FFT calculation*/
	void process(AudioBuffer<float>& continuousBuffer) override;

	/** Launch FFT calculation thread */
	bool startAcquisition() override;

	/** Stop FFT calculation thread*/
	bool stopAcquisition() override;

	/** Run FFT calculation in a separate thread */
	void run() override;
    
    /** Called when parameter value is updated*/
    void parameterValueChanged(Parameter* param) override;

	/** Called by the canvas to get the number of active chans*/
	Array<int> getActiveChans();

	/** Returns the name of the selected channel at a given index */
	const String getChanName(int localIdx);

	/** Sets the min/max frequency range*/
	void setFrequencyRange(Range<int>);

	/** Returns the frequency step for the currently selected range*/
	float getFreqStep() { return tfrParams.freqStep; };

	/** Holds incoming samples and outgoing powers */
	struct PowerBuffer
	{
		/** Incoming samples for each time step */
		OwnedArray<AtomicallyShared<FFTWArrayType>> incomingSamples;

		/** Outgoing power for each time step */
		OwnedArray<AtomicallyShared<std::vector<float>>> power;

		/** Write index */
		Array<int> writeIndex;

		/** Hamming window to apply to buffer */
		Array<float> window;

		/** Size of each buffer in samples */
		int bufferSize = 0;

		/** Step size in samples */
		int stepSize = 0;

		/** Steps per buffer samples */
		int stepsPerBuffer = 0;

		/** Number of fft frequencies */
		int nFreqs;

		/** Keep track of total samples written */
		long int totalSamplesWritten = 0;

		/** true if buffer size was updated */
		bool bufferSizeChanged = true;

		/** true if number of freqs was updated */
		bool numFreqsChanged = true;

		/** Changes buffer size*/
		void setBufferSize(int bufferSize_, int stepSize_)
		{
			if (bufferSize != bufferSize_)
			{
				bufferSize = bufferSize_;
				stepSize = stepSize_;
				stepsPerBuffer = bufferSize / stepSize;
				bufferSizeChanged = true;
			}
		}

		/** Changes num freqs */
		void setNumFreqs(int nFreqs_)
		{
			if (nFreqs != nFreqs_)
			{
				nFreqs = nFreqs_;
				numFreqsChanged = true;
			}
		}

		/** Resets all shared objects and indices */
		void reset()
		{
			for (int i = 0; i < stepsPerBuffer + 5; i++)
			{
				incomingSamples[i]->reset();
				power[i]->reset();
				writeIndex.set(i, -1 * i * stepSize);
				totalSamplesWritten = 0;
			}
		}

		/** Resizes all buffers */
		void resize()
		{
			if (bufferSizeChanged)
			{
				incomingSamples.clear();

				LOGD("Creating ", stepsPerBuffer + 5, " sample buffers of length ", bufferSize);

				for (int i = 0; i < stepsPerBuffer + 5; i++)
				{
					incomingSamples.add(new AtomicallyShared<FFTWArrayType>());
					incomingSamples.getLast()->map([=](FFTWArrayType& arr)
					{
						arr.resize(bufferSize);
					});
				}

				bufferSizeChanged = false;

				window.clear();

				const float N = float(bufferSize);
				const float PI = 3.1415926535;

				for (int n = 0; n < bufferSize; n++)
				{
					window.add(0.54 - 0.46 * cos(2 * PI * n / N));
				}
			}
			
			if (numFreqsChanged)
			{
				power.clear();

				LOGD("Creating ", stepsPerBuffer + 5, " power buffers of length ", nFreqs);

				for (int i = 0; i < stepsPerBuffer + 5; i++)
				{
					power.add(new AtomicallyShared<std::vector<float>>());
					power.getLast()->map([=](std::vector<float>& arr)
					{
						arr.resize(nFreqs);
					});
				}

				numFreqsChanged = false;
			}
		}
	};

	/** Array of buffers */
	PowerBuffer powerBuffers[MAX_CHANS];

	/** Type of visualization */
	DisplayType displayType;

private:

	/** Append FFTWArrays to data buffer */
	void updateDataBufferSize(int size);

	/** Change the size of the data buffer*/
	void updateDisplayBufferSize(int newSize);

	/** Returns true if a given stream ID is available*/
	bool streamExists(uint16 streamId);

	ScopedPointer<CumulativeTFR> TFR;

	/** Priority from 0 to 10 */
	static const int THREAD_PRIORITY = 5;

	/** Resets buffers*/
	void resetTFR();

	Array<int> channels;
	Array<Array<int>> bufferIdx; // channels x stepsPerBuffer

	//int bufferSize;
	//int stepSize;
	//int stepsPerBuffer;
    
    uint16 activeStream = 0;

	int numTrials;

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

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumViewer);
};

#endif // SPECTRUM_VIEWER_H_INCLUDED
