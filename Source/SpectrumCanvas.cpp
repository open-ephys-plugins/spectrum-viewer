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

#include "SpectrumCanvas.h"
#include <math.h>
#include <climits>

SpectrumCanvas::SpectrumCanvas(SpectrumViewer* n)
	: processor(n)
	, displayType(POWER_SPECTRUM)
{
    refreshRate = 60;

	canvasPlot = std::make_unique<CanvasPlot>(processor);

	viewport = std::make_unique<Viewport>();
	viewport->setViewedComponent(canvasPlot.get(), true);
	viewport->setScrollBarsShown(true, true);
	addAndMakeVisible(viewport.get());
}


void SpectrumCanvas::resized()
{
	int plotWidth, plotHeight;
	
	viewport->setBounds(0, 0, getWidth(), getHeight());
	
	if(displayType == POWER_SPECTRUM)
	{
		if(viewport->getMaximumVisibleWidth() < 840 + canvasPlot->legendWidth)
			plotWidth = 800;
		else
			plotWidth = viewport->getMaximumVisibleWidth() - canvasPlot->legendWidth - 40;
		
		if(viewport->getMaximumVisibleHeight() < 650)
			plotHeight = 600;
		else
			plotHeight = viewport->getMaximumVisibleHeight() - 50;
			
		canvasPlot->setBounds(0, 0, plotWidth + canvasPlot->legendWidth + 40, plotHeight + 50);
	}
	else
	{
		canvasPlot->setBounds(0, 0, viewport->getMaximumVisibleWidth(), viewport->getMaximumVisibleHeight());
	}

}

void SpectrumCanvas::refreshState() {}

void SpectrumCanvas::paint(Graphics& g)
{
	// g.fillAll(Colour(28,28,28));
}

void SpectrumCanvas::update()
{
	canvasPlot->updateActiveChans();
}

void SpectrumCanvas::refresh()
{
	//std::cout << "Refresh." << std::endl;

	bool needsRedraw = false;

	for (int i = 0; i < MAX_CHANS; i++)
	{
		SpectrumViewer::PowerBuffer* buffer = &processor->powerBuffers[i];

		for (int j = 0; j < buffer->power.size(); j++)
		{
			if (buffer->power[j]->hasUpdate())
			{
				AtomicScopedReadPtr<std::vector<float>> powerReader(*buffer->power[j]);

				
				powerReader.pullUpdate();

				if (powerReader.isValid())
				{
					//LOGD("Buffer ", j, " drawing spectrum");
					if (displayType == POWER_SPECTRUM)
					{
						needsRedraw = true;

						canvasPlot->updatePowerSpectrum(powerReader.operator*(), i);
					}
					else //Spectrogram
					{
						if (i == 0)
							canvasPlot->drawSpectrogram(powerReader.operator*());
					}
				}

			}
		}
	}

	if (needsRedraw)
		canvasPlot->plotPowerSpectrum();

}


void SpectrumCanvas::setDisplayType(DisplayType type)
{
	
	if(CoreServices::getAcquisitionStatus())
	{
		stopCallbacks();
		displayType = type;
		canvasPlot->setDisplayType(type);
		startCallbacks();
	}
	else
	{
		displayType = type;
		canvasPlot->setDisplayType(type);
	}

	resized();
}


/** CANVAS PLOT - Stores the plot along with it's legend*/

CanvasPlot::CanvasPlot(SpectrumViewer* p)
	: processor(p)
	, displayType(POWER_SPECTRUM)
	, freqStep(4)
	, nFreqs(250)
	, freqEnd(1000)
{
	plt.title("POWER SPECTRUM");
	XYRange range{ 0, 1000, 0, 5 };
	plt.setRange(range);
	plt.xlabel("Frequency (Hz)");
	plt.ylabel("Power");
	plt.setBackgroundColour(Colour(45,45,45));
	plt.setGridColour(Colour(100,100,100));
	plt.setInteractive(InteractivePlotMode::OFF);
	addAndMakeVisible(plt);

	activeChannels = processor->getActiveChans();

	spectrogramImg = std::make_unique<Image>(Image::RGB, 1000, 1000, true);
	setOpaque(true);

	currPower.resize(MAX_CHANS);

	for (int ch = 0; ch < MAX_CHANS; ch++)
	{
		currPower[ch].clear();
		lowPassFilters.add(new OwnedArray<Dsp::Filter>());
	}

	for (int i = 0; i < nFreqs; i++)
	{
		xvalues.push_back(i * freqStep);
	}
	
}


void CanvasPlot::resized()
{
	plt.setBounds(20, 30, getWidth() - legendWidth - 40, getHeight() - 50);
}

void CanvasPlot::updateActiveChans()
{
	activeChannels = processor->getActiveChans();
	repaint();
}

void CanvasPlot::setFrequencyRange(int freqStart_, int freqEnd_, float freqStep_)
{
	freqStep = freqStep_;
	freqEnd = freqEnd_;
	nFreqs = (int)(freqEnd_ - freqStart_)/freqStep_;

	xvalues.clear();
	for (int i = 0; i < nFreqs; i++)
	{
		xvalues.push_back(i * freqStep);
	}

	XYRange range{ (float)freqStart_, (float)freqEnd_, 0, 5 };
	plt.setRange(range);

	// Create a low pass filter for each frequency within each channel
	for (int ch = 0; ch < MAX_CHANS; ch++)
	{
		lowPassFilters[ch]->clear();
		for (int i = 0; i < nFreqs; i++)
		{
			lowPassFilters[ch]->add(new Dsp::SmoothedFilterDesign
							<Dsp::Butterworth::Design::LowPass    // design type
							<5>,                                   // order
							1,                                     // number of channels (must be const)
							Dsp::DirectFormII>(1));
			
			Dsp::Params params;
			params[0] = 50;                 		// sample rate (Hz)
			params[1] = 5;                          // order
			params[2] = 1;                         // cut-off frequency
			lowPassFilters[ch]->getLast()->setParams(params);
		}
	}
}

void CanvasPlot::setDisplayType(DisplayType type)
{
	displayType = type;

	if(displayType == SPECTROGRAM)
		plt.setVisible(false);
	else
		plt.setVisible(true);

	clear();
	repaint();
}

void CanvasPlot::plotPowerSpectrum()
{
	plt.clear();
	
	for (int i = 0; i < activeChannels.size(); i++)
	{

		if (maxPower > 0);
		{
			XYRange pltRange;
			plt.getRange(pltRange);

			if (pltRange.ymax < maxPower || (pltRange.ymax - maxPower) > 5)
			{
				pltRange.ymax = maxPower;
				plt.setRange(pltRange);
			}
		}

		plt.plot(xvalues, currPower[i], chanColors[i], 1.0f);
	}

	maxPower = -1.0f;

}

void CanvasPlot::updatePowerSpectrum(std::vector<float> powerData, int channelIndex)
{

	float lastPower = 0;
	currPower[channelIndex].clear();
	std::vector<float> powerBuffer;

	for (int n = 0; n < powerData.size(); n++)
	{

		if (powerData[n] > 0)
		{

			// Apply low pass filter for that frequency
			float* pData = &powerData[n];
			lowPassFilters[channelIndex]->getUnchecked(n)->process(1, &pData);

			if (*pData > 0)
			{
				float logP = log(*pData);

				if (logP > maxPower)
					maxPower = logP;

				powerBuffer.push_back(logP);
				//currPower.at(channelIndex).push_back(logP);
				lastPower = logP;

				//std::cout << logP << std::endl;
			}
			else
			{
				powerBuffer.push_back(lastPower);
				//currPower.at(channelIndex).push_back(lastPower);
			}
			
		}
		else
		{
			powerBuffer.push_back(lastPower);
			//currPower.at(channelIndex).push_back(lastPower);
		}
	}


	if (true)
	{
		float window[] = { 0.1111, 0.1111, 0.1111, 0.1111, 0.1111, 0.1111, 0.1111, 0.1111, 0.1111 };

		// apply smoothing
		for (int n = 0; n < powerBuffer.size(); n++)
		{

			float value = 0;

			for (int offset = -4; offset < 5; offset++)
			{
				int i = n + offset;

				if (i < 0)
				{
					i = 0;
				}
				else if (i >= powerBuffer.size())
				{
					i = powerBuffer.size() - 1;
				}

				value += (powerBuffer[i] * window[offset + 4]);

			}

			currPower.at(channelIndex).push_back(value);

		}
	}
	


}

void CanvasPlot::drawSpectrogram(std::vector<float> chanData)
{
	auto imageWidth = spectrogramImg->getWidth() - 1;
    auto imageHeight = spectrogramImg->getHeight();

	// first, shuffle our image rightwards by 1 pixel..
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth, imageHeight);

	// find the range of values produced, so we can scale our rendering to
    // show up the detail clearly
    auto powerRange = juce::FloatVectorOperations::findMinAndMax (chanData.data(), chanData.size());

	for (auto y = 0; y < imageHeight; ++y)
	{
		auto skewedProportionY = 1.0f - (float) y / (float) imageHeight;
		auto dataIndex = (size_t) jlimit (0, (int)(chanData.size() - 1), (int) (skewedProportionY * (chanData.size() - 1)));

		float logPower = log(chanData[dataIndex]);
		float logMax = log(powerRange.getEnd());
		float logMin = log(powerRange.getStart());
		
		if(chanData[dataIndex] < 1)
			logPower = 0;
		
		if(powerRange.getEnd() < 0)
			logMax = 0;
		
		if(powerRange.getStart() < 0)
			logMin = 0;

		auto level = juce::jmap (logPower, logMin, juce::jmax (logMax, 1e-5f), 0.0f, 1.0f);
		spectrogramImg->setPixelAt (0, y, juce::Colour::fromHSV (level, 1.0f, level, 1.0f));
	}

	repaint();
}

void CanvasPlot::paint(Graphics& g)
{
	g.fillAll(Colour(28,28,28));

	if(displayType == POWER_SPECTRUM)
	{
		if(activeChannels.size() ==  0)
			return;

		int left = getWidth() - legendWidth - 10;
		int top = 60;

		g.setFont(Font("Fira Code", "SemiBold", 16.0f));
		
		for(int i = 0; i < activeChannels.size(); i++)
		{
			top = (i+1) * rowHeight + 10;

			g.setColour(chanColors.at(i));
			g.fillRect( left
					, top + 10
					, 30
					, 30);

			g.setColour(Colours::lightgrey);
			String chan = processor->getChanName(activeChannels[i]);
			g.drawFittedText(chan
				, left + 45
				, top + 10
				, (legendWidth - 20) / 2
				, 30
				, Justification::centredLeft
				, 1);

			g.setColour(Colours::grey);
			g.drawRect(left, top + 10, 30, 30, 2);
				
		}
	}
	else
	{
        g.setColour(Colours::white);

		int w = 50;
		int h = getHeight();

		int padding = 10;

		g.drawLine(w-3, padding, w-3, h-padding, 2.0);

		int ticklabelWidth = 60;
		int tickLabelHeight = 20;


		for (int k = 0; k <= 10; k++)
		{
			float ytickloc = padding + ((k) * (h - padding*2) / 10);

			ytickloc = h - ytickloc;

			g.drawLine(w -13, ytickloc, w-3, ytickloc, 2.0);

			String yTick;

			if(k != 0)
				yTick = String((freqEnd * k) / 10);
			else
				yTick = String(0);
				
			
			g.drawText(yTick, 
						0, ytickloc - tickLabelHeight / 2, 
						w - 15, 
						tickLabelHeight, 
						Justification::right, 
						false);

		}
		
		auto imgBounds = getLocalBounds();
		imgBounds.setLeft(60);
		imgBounds.setRight(getWidth() - 10);
		imgBounds.setBottom(getHeight() - 10);
		imgBounds.setTop(10);
		g.drawImage (*spectrogramImg, imgBounds.toFloat());
    }
}

void CanvasPlot::clear()
{
	for (int ch = 0; ch < MAX_CHANS; ch++)
	{
		currPower[ch].clear();

		for (int i = 0; i < nFreqs; i++)
			lowPassFilters[ch]->getUnchecked(i)->reset();
	}

	lowPassFilters[0]->getUnchecked(0)->reset();

	spectrogramImg->clear(spectrogramImg->getBounds());
	plt.clear();
}