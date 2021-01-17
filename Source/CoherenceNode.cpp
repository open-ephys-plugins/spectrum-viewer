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

#include "CoherenceNode.h"
#include "CoherenceNodeEditor.h"
/********** node ************/
CoherenceNode::CoherenceNode()
	: GenericProcessor("Spectrum Viewer")
	, Thread("FFT Thread")
	, nSamplesWait(0)
    , displayType(POWER_SPECTRUM)
{
	setProcessorType(PROCESSOR_TYPE_SINK);

	tfrParams.segLen = 0.25;
	tfrParams.freqStart = 4;
	tfrParams.freqEnd = 1000;
	tfrParams.stepLen = 0.1;
	tfrParams.winLen = 0.25;
	tfrParams.interpRatio = 1;
	tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);
	tfrParams.Fs = 2000;
	tfrParams.alpha = 0;

	channels.add(0);
	channels.add(1);

	bufferIdx.add(0);
	bufferIdx.add(0);

	sampleIdx.add(0);
	sampleIdx.add(0);

	lastValue.add(0.0f);
	lastValue.add(0.0f);
}

CoherenceNode::~CoherenceNode()
{}

AudioProcessorEditor* CoherenceNode::createEditor()
{
	editor = new CoherenceEditor(this);
	return editor;
}

void CoherenceNode::process(AudioSampleBuffer& continuousBuffer)
{

	///// Add incoming data to data buffer. ////
	AtomicScopedWritePtr<Array<FFTWArrayType>> dataWriter(dataBuffer);

	// Check writer
	if (!dataWriter.isValid())
	{
		jassertfalse; // atomic sync data writer broken
	}

	//for loop over active channels and update buffer with new data
	int nSamples = getNumSamples(channels[0]); // all channels the same?
	int maxSamples = dataWriter->getReference(0).getLength();

	bool updateBuffer = false;
	int samplesAdded[2] = { 0, 0 };

	for (int i = 0; i < 2; i++)
	{

		if (nSamples == 0 || channels[i] < 0)
		{
			continue;
		}

		const float* incomingDataPointer = continuousBuffer.getReadPointer(channels[i]);

		float value = lastValue[i];

		// loop through available samples
		for (int n = 0; n < nSamples; n++)
		{
			if ((sampleIdx[i] % downsampleFactor) < (downsampleFactor -1)) // average adjacent samples
			{
				//std::cout << "skip" << std::endl;
				value += incomingDataPointer[n];
				sampleIdx.set(i, sampleIdx[i] + 1);
				samplesAdded[i]++;
				lastValue.set(i, value);
			}
			else
			{
				//std::cout << " " << std::endl;
				value += incomingDataPointer[n];
				value /= downsampleFactor;
				dataWriter->getReference(i).set(bufferIdx[i], value);
				bufferIdx.set(i, bufferIdx[i] + 1);
				sampleIdx.set(i, 0);
				value = 0;
				samplesAdded[i]++;
				lastValue.set(i, value);

				if (bufferIdx[i] == maxSamples)
				{
					updateBuffer = true;
					break;
				}
					
			}
		}

	}

	// channel buf is full. Update buffer.
	if (updateBuffer)
	{
		//std::cout << "Pushing buffer update" << std::endl;
		dataWriter.pushUpdate();
		// Reset samples added and increment trial number
		bufferIdx.set(0, 0);
		bufferIdx.set(1, 0);
		numTrials++;
		updateBuffer = false;
	}

	if (samplesAdded[0] < nSamples)
	{
		for (int i = 0; i < 2; i++)
		{

			if (nSamples == 0 || channels[i] < 0)
			{
				continue;
			}

			const float* incomingDataPointer = continuousBuffer.getReadPointer(channels[i]);

			float value = lastValue[i];

			// loop through available samples
			for (int n = samplesAdded[i]; n < nSamples; n++)
			{
				if ((sampleIdx[i] % downsampleFactor) > 0) // average adjacent samples
				{
					value += incomingDataPointer[n];
					sampleIdx.set(i, sampleIdx[i] + 1);
					samplesAdded[i]++;
					lastValue.set(i, value);
				}
				else
				{
					//value += incomingDataPointer[n];
					//value /= downsampleFactor;

					value = incomingDataPointer[n];
					dataWriter->getReference(i).set(bufferIdx[i], value);
					bufferIdx.set(i, bufferIdx[i] + 1);
					sampleIdx.set(i, 0);
					value = 0;
					samplesAdded[i]++;
					lastValue.set(i, value);

					if (bufferIdx[i] == maxSamples)
					{
						updateBuffer = true;
						break;
					}

				}
			}

		}
	}

	// channel buf is full. Update buffer.
	if (updateBuffer)
	{
		//std::cout << "Pushing buffer update" << std::endl;
		dataWriter.pushUpdate();
		// Reset samples added and increment trial number
		bufferIdx.set(0, 0);
		bufferIdx.set(1, 0);
		numTrials++;
		updateBuffer = false;
	}

	
}

void CoherenceNode::run()
{
	AtomicScopedReadPtr<Array<FFTWArrayType>> dataReader(dataBuffer);

	while (!threadShouldExit())
	{
		//// Check for new filled data buffer and run stats ////        
		if (dataBuffer.hasUpdate())
		{
			//std::cout << "Got buffer update, adding trial" << std::endl;
			dataReader.pullUpdate();

			for (int activeChan = 0; activeChan < 2; activeChan++)
			{
				//std::cout << "Adding channel " << activeChan << std::endl;
				TFR->addTrial(dataReader->getReference(activeChan), activeChan);
			}

			if (displayType == POWER_SPECTRUM)
				TFR->getPowerForChannels(power);

			else if (displayType == COHERENCE)
				TFR->getMeanCoherence(0, 1, coherence, 0); // check "comb" value
		}
	}
}



uint32 CoherenceNode::getDataSubprocId(int chan)
{
	return getChannelSourceId(getDataChannel(chan));
}

uint32 CoherenceNode::getChannelSourceId(const InfoObjectCommon* chan)
{
	return getProcessorFullId(chan->getSourceNodeID(), chan->getSubProcessorIdx());
}


void CoherenceNode::updateSettings()
{

}

void CoherenceNode::updateSubprocessor()
{
	if ((channels[0] > -1) || (channels[1] > -1))
	{
		float Fs; 

		if (channels[0] > -1)
			Fs = getDataChannel(channels[0])->getSampleRate();
		else
			Fs = getDataChannel(channels[1])->getSampleRate();

		downsampleFactor = (int) Fs / targetRate;

		int bufferSize = int(Fs / downsampleFactor * tfrParams.winLen);

		std::cout << "Updating data buffers for " << Fs << " Hz, downsample factor of " << downsampleFactor << std::endl;
		updateDataBufferSize(bufferSize, bufferSize);

		tfrParams.Fs = Fs / downsampleFactor;

		// (Start - end freq) / stepsize
		tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);

		//freqStep = 1; // for debugging
		tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep) + 1;

		std::cout << "Updating display buffers to " << tfrParams.nFreqs << " frequencies." << std::endl;
		updateDisplayBufferSize(tfrParams.nFreqs);

		resetTFR();
	}
}

void CoherenceNode::updateDataBufferSize(int size1, int size2)
{
	// no writers or readers can exist here
	// so this can't be called during acquisition

	dataBuffer.map([=](Array<FFTWArrayType>& arr)
	{
		std::cout << "Resizing data buffer to " << size1 << ", " << size2 << std::endl;
		arr.resize(2);
		arr.getReference(0).resize(size1);
		arr.getReference(1).resize(size2);
	});

	///// Add incoming data to data buffer. Let thread get the ok to start at 8 seconds of data ////
	AtomicScopedWritePtr<Array<FFTWArrayType>> dataWriter(dataBuffer);
	// Check writer
	if (!dataWriter.isValid())
	{
		jassertfalse; // atomic sync data writer broken
	}


	int maxSamples = dataWriter->getReference(0).getLength();

	std::cout << "TOTAL BUFFER SIZE: " << maxSamples << std::endl;

}

void CoherenceNode::updateDisplayBufferSize(int newSize)
{
	power.map([=](std::vector<std::vector<float>>& vec)
	{
		std::cout << "Resizing power buffer to " << newSize << ", " << newSize << std::endl;
		vec.resize(2);
		vec[0].resize(newSize);
		vec[1].resize(newSize);
	});

	coherence.map([=](std::vector<double>& vec)
	{
		std::cout << "Resizing coherence buffer to " << newSize << ", " << newSize << std::endl;
		vec.resize(newSize);
	});
}



void CoherenceNode::setParameter(int parameterIndex, float newValue)
{
	if (settings.numInputs < 0)
		return;

	switch (parameterIndex)
	{
	case 0:
		channels.set(0, int(newValue));
		std::cout << "Setting channel A to " << int(newValue) << std::endl;
		break;
	case 1:
		channels.set(1, int(newValue));
		std::cout << "Setting channel B to " << int(newValue) << std::endl;
		break;
	}
}


void CoherenceNode::resetTFR()
{
	
	nSamplesAdded = 0;

	// Trim time close to edge
	int nSamplesWin = tfrParams.winLen * tfrParams.Fs;

	tfrParams.nTimes = 1;// ((tfrParams.segLen * tfrParams.Fs) -
					   // (nSamplesWin)) / tfrParams.Fs *
						//(1 / tfrParams.stepLen) + 1; // Trim half of window on both sides, so 1 window length is trimmed total

	std::cout << "Total times: " << tfrParams.nTimes << std::endl;

	std::cout << "nfft: " << int(tfrParams.Fs * tfrParams.segLen) << std::endl;

	TFR = new CumulativeTFR(1, // group 1 channel count
		1, // group 2 channel count
		tfrParams.nFreqs, 
		tfrParams.nTimes, 
		tfrParams.Fs, // 2000
		tfrParams.winLen, 
		tfrParams.stepLen,
		tfrParams.freqStep, 
		tfrParams.freqStart, 
		tfrParams.segLen, //fftSec
		tfrParams.alpha);

}



bool CoherenceNode::enable()
{
	if (isEnabled)
	{
		// Start coherence calculation thread
		numTrials = 0;
		//numArtifacts = 0;
		startThread(THREAD_PRIORITY);

		for (int i = 0; i < 2; i++)
		{
			bufferIdx.set(i, 0);
			lastValue.set(i, 0.0f);
			sampleIdx.set(i, 0);
		}
	}
	return isEnabled;
}

bool CoherenceNode::disable()
{
	CoherenceEditor* editor = static_cast<CoherenceEditor*>(getEditor());
	editor->disable();

	signalThreadShouldExit();

	return true;
}


bool CoherenceNode::hasEditor() const
{
	return true;
}

