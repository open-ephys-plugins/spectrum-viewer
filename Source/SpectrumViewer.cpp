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
	, displayType(POWER_SPECTRUM)
{
	tfrParams.segLen = 1;
	tfrParams.freqStart = 0;
	tfrParams.freqEnd = 1000;
	tfrParams.stepLen = 0.020; // update every 20 ms (50 Hz)
	tfrParams.winLen = 0.25;
	tfrParams.interpRatio = 1;
	tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);
	tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);
	tfrParams.Fs = 2000;
	tfrParams.alpha = 0;
	tfrParams.nTimes = 1;

	addIntParameter(Parameter::GLOBAL_SCOPE,
		"active_stream", "Currently selected stream",
		0, 0, 200000, true);

	addSelectedChannelsParameter(Parameter::STREAM_SCOPE, 
		"Channels", 
		"The channels to analyze", 
		MAX_CHANS, 
		false);

	bufferResizer = std::make_unique<BufferResizer>(this);
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

			tfrParams.Fs = getDataStream(activeStream)->getSampleRate();
			tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);
			tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

			int bufferSize = int(tfrParams.Fs * tfrParams.winLen);

			for (int i = 0; i < MAX_CHANS; i++)
			{
				powerBuffers[i].setBufferSize(bufferSize, tfrParams.stepLen * tfrParams.Fs);
				powerBuffers[i].setNumFreqs(tfrParams.nFreqs);
			}

			bufferResizer->resize();

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

void SpectrumViewer::setFrequencyRange(Range<int> newRange)
{
	if(newRange.getEnd() != tfrParams.freqEnd)
	{
		tfrParams.freqEnd = newRange.getEnd();
		
		if(tfrParams.freqEnd == 100)
			tfrParams.winLen = 2;
		else if(tfrParams.freqEnd == 500)
			tfrParams.winLen = 0.5;
		else if(tfrParams.freqEnd == 1000)
			tfrParams.winLen = 0.25;
		else
			tfrParams.winLen = 0.1;

		tfrParams.freqStep = 1.0 / float(tfrParams.winLen * tfrParams.interpRatio);
		tfrParams.nFreqs = int((tfrParams.freqEnd - tfrParams.freqStart) / tfrParams.freqStep);

		int bufferSize = int(tfrParams.Fs * tfrParams.winLen);

		for (int i = 0; i < MAX_CHANS; i++)
		{
			powerBuffers[i].setBufferSize(bufferSize, tfrParams.stepLen * tfrParams.Fs);
			powerBuffers[i].setNumFreqs(tfrParams.nFreqs);
		}

		bufferResizer->resize();
		resetTFR();

		getEditor()->updateVisualizer();
	}
}

void SpectrumViewer::process(AudioBuffer<float>& continuousBuffer)
{
	// Nothing to do when no channels selected
	if(channels.isEmpty())
		return;

	// same number of samples for all channels in stream
	int incomingSampleCount = getNumSamplesInBlock(activeStream);

	//bool updateBuffer = false;

	// loop over active channels
	for (int i = 0; i < channels.size(); i++)
	{

		int globalChanIdx = getGlobalChannelIndex(activeStream, channels[i]);
		const float* incomingDataPointer = continuousBuffer.getReadPointer(globalChanIdx);

		if (globalChanIdx < 0)
			continue;

		PowerBuffer* buffer = &powerBuffers[i];

		// loop over buffers
		for (int j = 0; j < buffer->incomingSamples.size(); j++)
		{

			AtomicScopedWritePtr<FFTWArrayType> dataWriter(*buffer->incomingSamples[j]);
			int writeIndex = buffer->writeIndex[j];

			//LOGD("Buffer ", j, " write index: ", writeIndex);

			// Check writer
			if (!dataWriter.isValid())
			{
				jassertfalse; // atomic sync data writer broken
			}

			// loop over samples
			for (int n = 0; n < incomingSampleCount; n++)
			{

				writeIndex++;

				if (writeIndex > 0) // make sure we have enough samples
				{
					dataWriter->set(writeIndex - 1, incomingDataPointer[n]);
				}

				if (writeIndex == buffer->bufferSize)
				{

					// apply window

					for (int m = 0; m < buffer->bufferSize; m++)
					{
						dataWriter->set(m, dataWriter->getAsReal(m) * buffer->window[m]);
					}
					dataWriter.pushUpdate();
					//LOGD("Buffer ", j, " is full.");
					writeIndex = -5 * buffer->stepSize; // loop around to the beginning
					break;
				}
			}

			buffer->writeIndex.set(j, writeIndex);

		}
	}
}

void SpectrumViewer::run()
{
	

	while (!threadShouldExit())
	{

		// loop over active channels
		for (int i = 0; i < channels.size(); i++)
		{

			PowerBuffer* buffer = &powerBuffers[i];

			// loop over buffers
			for (int j = 0; j < buffer->incomingSamples.size(); j++)
			{

				if (buffer->incomingSamples[j]->hasUpdate())
				{

					//LOGD("Buffer ", j, " has update.");

					AtomicScopedReadPtr<FFTWArrayType> fftReader(*buffer->incomingSamples[j]);
					AtomicScopedWritePtr<FFTWArrayType> fftWriter(*buffer->incomingSamples[j]);
					AtomicScopedWritePtr<std::vector<float>> powerWriter(*buffer->power[j]);

					if (fftReader.isValid() && fftWriter.isValid() && powerWriter.isValid())
					{
						fftReader.pullUpdate();

						TFR->computeFFT(fftWriter.operator*(), i);
						TFR->getPower(powerWriter.operator*(), i);
						
						powerWriter.pushUpdate();

						//LOGD("Buffer ", j, " computed FFT.");
					}

					
				}

			}

		}

		
	}
}

void SpectrumViewer::updateSettings()
{

	if(getDataStreams().size() == 0)
	{
		activeStream = 0;
		channels.clear();
	}

}


void SpectrumViewer::resetTFR()
{

	TFR.reset(new CumulativeTFR(MAX_CHANS, // channel count
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

		bufferResizer->waitForThreadToExit(5000);

		for (int i = 0; i < MAX_CHANS; i++)
		{
			powerBuffers[i].reset();
		}

		startThread(THREAD_PRIORITY);

	}
	return isEnabled;
}

bool SpectrumViewer::stopAcquisition()
{
	stopThread(1000);
	return true;
}


BufferResizer::BufferResizer(SpectrumViewer* p)
	: Thread("Spectrum Viewer buffer resizer")
	, processor(p)
{
	//setStatusMessage("Resizing buffers...");
}

void BufferResizer::resize()
{
	waitForThreadToExit(5000);

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