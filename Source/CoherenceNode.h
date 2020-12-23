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

#ifndef COHERENCE_NODE_H_INCLUDED
#define COHERENCE_NODE_H_INCLUDED

/*

Coherence Node - continuously compute and display magnitude-squared coherence
(measure of phase synchrony) between pairs of LFP signals for a set of frequencies
of interest. Displays either raw coherence values or change from a saved baseline,
in units of z-score.

*/

#include <ProcessorHeaders.h>
#include <VisualizerEditorHeaders.h>

#include "AtomicSynchronizer.h"
#include "CumulativeTFR.h"

#include <time.h>
#include <vector>
#include <chrono>
#include <ctime> 
#include <iostream>
#include<fstream>

enum DisplayType {
	COHEROGRAM = 3,
	SPECTROGRAM = 2,
	COHERENCE = 1,
	POWER_SPECTRUM = 0
};

class CoherenceNode : public GenericProcessor, public Thread
{
	friend class CoherenceEditor;
	friend class CoherenceVisualizer;
public:
	CoherenceNode();
	~CoherenceNode();

	bool hasEditor() const override;

	AudioProcessorEditor* createEditor() override;

	void setParameter(int parameterIndex, float newValue) override;

	void process(AudioSampleBuffer& continuousBuffer) override;

	//bool isReady() override;
	bool enable() override;
	bool disable() override;

	// thread function - coherence calculation
	void run() override;

	// Handle changing channels/groups
	void updateSettings() override;

	// Variable to store incoming data
	AtomicallyShared<Array<FFTWArrayType>> dataBuffer;

	// Array of powers across frequencies, for multiple channels
	AtomicallyShared<std::vector<std::vector<float>>> power;

	// Array of coherence values across frequencies
	AtomicallyShared<std::vector<double>> coherence;

	DisplayType displayType;

	// Save info
	void saveCustomParametersToXml(XmlElement* parentElement) override;
	void loadCustomParametersFromXml();

private:

	ScopedPointer<CumulativeTFR> TFR;

	// Append FFTWArrays to data buffer
	void updateDataBufferSize(int size1, int size2);

	void updateDisplayBufferSize(int newSize);

	int nSamplesAdded; // holds how many samples were added for each channel
	AudioBuffer<float> channelData; // Holds the segment buffer for each channel.
	int nSamplesWait; // How many seconds to wait after an artifact is seen.
	int nSamplesWaited; // Holds how many samples we've waited after an artifact. (wait 1 second before getting data again)

	// Total Combinations
	int nGroupCombs;

	// from 0 to 10
	static const int THREAD_PRIORITY = 5;

	void resetTFR();

	Array<int> channels;
	Array<int> bufferIdx;
	Array<int> sampleIdx;
	Array<float> lastValue;

	const float targetRate = 2000;

	int downsampleFactor = 1;

	int numTrials;

	// This is to store data in case of switch and we wish to retrive old data
	AtomicallyShared<Array<FFTWArrayType>> dataBufferII;

	enum ParameterType
	{
		SEGMENT_LENGTH,
		WINDOW_LENGTH,
		START_FREQ,
		END_FREQ,
		STEP_LENGTH,
		ARTIFACT_THRESHOLD
	};

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

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CoherenceNode);
};

#endif // COHERENCE_NODE_H_INCLUDED