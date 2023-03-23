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
class BufferResizer : public ThreadWithProgressWindow
{
public:

	/** Constructor */
	BufferResizer(int dataBufferSize, int displayBufferSize, SpectrumViewer* processor);

	void run() override;

private:

	int dataBuffersize;
	int displayBufferSize;
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

	const String getChanName(int localIdx);

	void setFrequencyRange(Range<int>);

	float getFreqStep() { return tfrParams.freqStep; };

	/** Variable to store incoming data */
	Array<AtomicallyShared<FFTWArrayType>> updatedDataBuffer;
	AtomicallyShared<Array<FFTWArrayType>> dataBuffer;

	/** Array of powers across frequencies, for multiple channels */
	AtomicallyShared<std::vector<std::vector<float>>> power;

	/** Type of visualization (currently only POWER_SPECTRUM is supported) */
	DisplayType displayType;

private:

	/** Append FFTWArrays to data buffer */
	void updateDataBufferSize(int size);

	/** Change the size of the data buffer*/
	void updateDisplayBufferSize(int newSize);

	bool streamExists(uint16 streamId);

	ScopedPointer<CumulativeTFR> TFR;

	int nSamplesToAdd; // holds how many samples to add for each channel before sending an update
	Array<int> samplesAdded; // samples added to each channel so far.

	// from 0 to 10
	static const int THREAD_PRIORITY = 5;

	void resetTFR();

	Array<int> channels;
	Array<int> bufferIdx;

	int bufferSize;
    
    uint16 activeStream = 0;

	int numTrials;

	// This is to store data in case of switch and we wish to retrive old data
	AtomicallyShared<Array<FFTWArrayType>> dataBufferII;

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

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumViewer);
};

#endif // SPECTRUM_VIEWER_H_INCLUDED
