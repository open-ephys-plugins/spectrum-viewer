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

#define MS_FROM_START Time::highResolutionTicksToSeconds(Time::getHighResolutionTicks() - start) * 1000

SpectrumViewer::SpectrumViewer()
	: GenericProcessor("Spectrum Viewer")
	, Thread("FFT Thread")
	, bufferSize(0)
	, displayType(POWER_SPECTRUM)
{
	tfrParams.segLen = 1;
	tfrParams.freqStart = 4;
	tfrParams.freqEnd = 1000;
	tfrParams.stepLen = 0.1;
	tfrParams.winLen = 0.25;
	tfrParams.interpRatio = 1;
	tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);
	tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep) + 1;
	tfrParams.Fs = 2000;
	tfrParams.alpha = 0;

	addIntParameter(Parameter::GLOBAL_SCOPE,
		"active_stream", "Currently selected stream",
		0, 0, 200000, false);

	addSelectedChannelsParameter(Parameter::STREAM_SCOPE, "Channels", "The channels to analyze", MAX_CHANS);

	bufferIdx.resize(MAX_CHANS);
}

AudioProcessorEditor* SpectrumViewer::createEditor()
{
	editor = std::make_unique<SpectrumViewerEditor>(this);
	return editor.get();
}

void SpectrumViewer::parameterValueChanged(Parameter* param)
{
	if (param->getName().equalsIgnoreCase("active_stream"))
	{

		uint16 candidateStream = (uint16) (int) param->getValue();

		if (streamExists(candidateStream))
		{
			activeStream = candidateStream;

			float Fs = getDataStream(activeStream)->getSampleRate();

			bufferSize = int(Fs * tfrParams.winLen);

			updateDataBufferSize(bufferSize);

			tfrParams.Fs = Fs;

			// (Start - end freq) / stepsize
			tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);

			//freqStep = 1; // for debugging
			tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep) + 1;

			updateDisplayBufferSize(tfrParams.nFreqs);

			resetTFR();
		}

	}
	else if (param->getName() == "Channels")
	{
		channels.clear();
		
		SelectedChannelsParameter* p = (SelectedChannelsParameter*) param;
		
		for (auto i : p->getArrayValue())
		{
			channels.add(int(i));
		}

		getEditor()->updateVisualizer();
	}
}

void SpectrumViewer::process(AudioBuffer<float>& continuousBuffer)
{
	// Nothing to do when no channels selected
	if(channels.isEmpty())
		return;

	///// Add incoming data to data buffer. ////
	AtomicScopedWritePtr<Array<FFTWArrayType>> dataWriter(dataBuffer);

	// Check writer
	if (!dataWriter.isValid())
	{
		jassertfalse; // atomic sync data writer broken
	}

	//for loop over active channels and update buffer with new data
	int nSamples = getNumSamplesInBlock(activeStream); // all channels the same?

	bool updateBuffer = false;
	Array<int> samplesAdded;
	samplesAdded.insertMultiple(0, 0, channels.size());

	for (int i = 0; i < channels.size(); i++)
	{
		int globalChanIdx = getGlobalChannelIndex(activeStream, channels[i]);

		if (nSamples == 0 || globalChanIdx < 0)
		{
			continue;
		}

		// Copy BufferSize - NumSamples data from previous buffer
		if(bufferIdx[i] == bufferSize)
		{
			auto dataPointer = dataWriter->getReference(i).getReadPointer(nSamples);
			for(int samp = 0; samp < bufferSize - nSamples; samp++)
				dataWriter->getReference(i).set(samp, dataPointer[samp]);
		}

		const float* incomingDataPointer = continuousBuffer.getReadPointer(globalChanIdx);

		// loop through available samples
		for (int n = 0; n < nSamples; n++)
		{
			if(bufferIdx[i] < bufferSize)
			{
				dataWriter->getReference(i).set(bufferIdx[i], incomingDataPointer[n]);
				bufferIdx.set(i, bufferIdx[i] + 1);

				if(bufferIdx[i] == bufferSize)
				{
					samplesAdded.set(i, samplesAdded[i] + 1);
					break;
				}
			}
			else
			{
				dataWriter->getReference(i).set((bufferSize - nSamples) + n, incomingDataPointer[n]);
			}

			samplesAdded.set(i, samplesAdded[i] + 1);

		}

	}

	// Add remaining samples (if any) after pushing update
	if (samplesAdded[0] < nSamples)
	{
		dataWriter.pushUpdate();

		for (int i = 0; i < channels.size(); i++)
		{
			int globalChanIdx = getGlobalChannelIndex(activeStream, channels[i]);

			if (nSamples == 0 || globalChanIdx < 0)
			{
				continue;
			}

			int samplesLeft = nSamples - samplesAdded[i]; 

			// Copy BufferSize - NumSamples data from previous buffer
			auto dataPointer = dataWriter->getReference(i).getReadPointer(samplesLeft);
			for(int samp = 0; samp < bufferSize - samplesLeft; samp++)
				dataWriter->getReference(i).set(samp, dataPointer[samp]);

			const float* incomingDataPointer = continuousBuffer.getReadPointer(globalChanIdx);

			// loop through available samples
			for (int n = samplesAdded[i]; n < nSamples; n++)
			{
				dataWriter->getReference(i).set((bufferSize - samplesLeft) + n, incomingDataPointer[n]);
				// if (bufferIdx[i] == bufferSize)
				// {
				// 	updateBuffer = true;
				// 	break;
				// }
			}

		}
	}

	// channel buf is full. Update buffer.
	if (bufferIdx[0] == bufferSize)
	{
		//std::cout << "Pushing buffer update" << std::endl;
		dataWriter.pushUpdate();
		// // Reset samples added and increment trial number
		// bufferIdx.set(0, 0);
		// bufferIdx.set(1, 0);
		// numTrials++;
		// updateBuffer = false;
	}

	
}

void SpectrumViewer::run()
{
	AtomicScopedReadPtr<Array<FFTWArrayType>> dataReader(dataBuffer);

	while (!threadShouldExit())
	{
		//// Check for new filled data buffer and run stats ////
		if (dataBuffer.hasUpdate())
		{
			//std::cout << "Got buffer update, adding trial" << std::endl;
			dataReader.pullUpdate();

			for (int activeChan = 0; activeChan < channels.size(); activeChan++)
			{
				//std::cout << "Adding channel " << activeChan << std::endl;
				auto chanData = dataReader->getUnchecked(activeChan);
				TFR->addTrial(chanData, activeChan);
			}

			TFR->getPowerForChannels(power);

		}
	}
}

void SpectrumViewer::updateSettings()
{

	// activeStream = 0;

	// if (dataStreams.size() > 0)
	// {
	// 	activeStream = dataStreams[0]->getStreamId();
	// 	getParameter("active_stream")->setNextValue(activeStream);
	// }

}

void SpectrumViewer::updateDataBufferSize(int size)
{
	// no writers or readers can exist here
	// so this can't be called during acquisition
	dataBuffer.reset();

	dataBuffer.map([=](Array<FFTWArrayType>& arr)
	{
		LOGD("Resizing data buffer to ", size);
		arr.resize(MAX_CHANS);

		for(int i = 0; i < MAX_CHANS; i++)
			arr.getReference(i).resize(size);
	});

	///// Add incoming data to data buffer. Let thread get the ok to start at 8 seconds of data ////
	AtomicScopedWritePtr<Array<FFTWArrayType>> dataWriter(dataBuffer);
	// Check writer
	if (!dataWriter.isValid())
	{
		jassertfalse; // atomic sync data writer broken
	}

}

void SpectrumViewer::updateDisplayBufferSize(int newSize)
{
	power.reset();
	
	power.map([=](std::vector<std::vector<float>>& vec)
	{
		LOGD("Resizing power buffer to ", newSize);
		vec.resize(MAX_CHANS);

		for(int i = 0; i < MAX_CHANS; i++)
			vec[i].resize(newSize);

	});

	// coherence.map([=](std::vector<double>& vec)
	// {
	// 	std::cout << "Resizing coherence buffer to " << newSize << ", " << newSize << std::endl;
	// 	vec.resize(newSize);
	// });
}


void SpectrumViewer::resetTFR()
{
	
	nSamplesAdded = 0;

	// Trim time close to edge
	int nSamplesWin = tfrParams.winLen * tfrParams.Fs;

	tfrParams.nTimes = 1;// ((tfrParams.segLen * tfrParams.Fs) -
					   // (nSamplesWin)) / tfrParams.Fs *
						//(1 / tfrParams.stepLen) + 1; // Trim half of window on both sides, so 1 window length is trimmed total

	TFR.reset(new CumulativeTFR(MAX_CHANS, // channel count
		tfrParams.nFreqs, 
		tfrParams.nTimes, 
		tfrParams.Fs, // 2000
		tfrParams.winLen, 
		tfrParams.stepLen,
		tfrParams.freqStep, 
		tfrParams.freqStart, 
		tfrParams.segLen, //fftSec
		tfrParams.alpha));

}

bool SpectrumViewer::streamExists(uint16 streamId)
{
	for (auto stream : getDataStreams())
	{
		if (stream->getStreamId() == streamId)
			return true;
	}

	return false;
}

Array<int> SpectrumViewer::getActiveChans()
{
	return channels;
}

const String SpectrumViewer::getChanName(int localIdx)
{
	return getDataStream(activeStream)->getContinuousChannels()[localIdx]->getName();
};

bool SpectrumViewer::startAcquisition()
{
	if (isEnabled)
	{
		startThread(THREAD_PRIORITY);

		for (int i = 0; i < MAX_CHANS; i++)
		{
			bufferIdx.set(i, 0);
		}
	}
	return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
	stopThread(1000);

	return true;
}

