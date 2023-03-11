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
	, numActiveChans(1)
	, displayType(POWER_SPECTRUM)
{
	refreshRate = 60;
	
	plt.title("POWER SPECTRUM");
	XYRange range{ 0, 1000, 0, 5 };
	plt.setRange(range);
	plt.xlabel("Frequency (Hz)");
	plt.ylabel("Power");
	plt.setBackgroundColour(Colours::grey);
	plt.setGridColour(Colours::darkgrey);
	plt.setInteractive(InteractivePlotMode::OFF);

	canvasPlot = std::make_unique<CanvasPlot>(processor);
	canvasPlot->addAndMakeVisible(plt);

	viewport = std::make_unique<Viewport>();
	viewport->setViewedComponent(canvasPlot.get(), false);
	viewport->setScrollBarsShown(true, true);
	addAndMakeVisible(viewport.get());

	nFreqs = processor->tfrParams.nFreqs;
	freqStep = processor->tfrParams.freqStep;

	currPower.resize(MAX_CHANS);
	prevPower.resize(MAX_CHANS);

	for (int ch = 0; ch < MAX_CHANS; ch++)
	{
		currPower[ch].clear();
		prevPower[ch].clear();
	}

	for (int i = 0; i < nFreqs; i++)
	{
		xvalues.push_back(i * freqStep);
	}
	
}


void SpectrumCanvas::resized()
{
	int plotWidth, plotHeight;
	
	viewport->setBounds(0, 0, getWidth(), getHeight());
	
	if(displayType == POWER_SPECTRUM)
	{
		if(viewport->getMaximumVisibleWidth() < 840 + canvasPlot->legendThickness)
			plotWidth = 800;
		else
			plotWidth = viewport->getMaximumVisibleWidth() - canvasPlot->legendThickness - 40;
		
		if(viewport->getMaximumVisibleHeight() < 640)
			plotHeight = 600;
		else
			plotHeight = viewport->getMaximumVisibleHeight() - 40;
			
		canvasPlot->setBounds(0, 0, plotWidth + canvasPlot->legendThickness + 40, plotHeight + 40);
		plt.setBounds(20, 30, plotWidth, plotHeight);
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
	plt.clear();

	canvasPlot->activeChannels = processor->getActiveChans();

	if(CoreServices::getAcquisitionStatus())
	{
		stopCallbacks();
		numActiveChans = canvasPlot->activeChannels.size();
		startCallbacks();
	}
	else
	{
		numActiveChans = canvasPlot->activeChannels.size();
		XYRange newRange = { 0, 1000, 0, 5 };
		plt.setRange(newRange); 
	}

	canvasPlot->clearSpectrogram();
	canvasPlot->repaint();
}

void SpectrumCanvas::refresh()
{
	//std::cout << "Refresh." << std::endl;
	
	if (processor->power.hasUpdate())
	{
		//std::cout << "Reading power data" << std::endl;

		AtomicScopedReadPtr<std::vector<std::vector<float>>> powerReader(processor->power);

		powerReader.pullUpdate();

		if (powerReader.isValid())
		{
			if(displayType == POWER_SPECTRUM)
			{
				plt.clear();

				float maxpower = -1;

				for (int ch = 0; ch < numActiveChans; ch++)
				{
					// LOGC("********** Buffer Index: ", bufferIndex[ch]);
					for (int n = 0; n < nFreqs; n++)
					{
						float p = powerReader->at(ch)[n];

						if (p > maxpower)
							maxpower = p;

						if (p > 0)
						{
							if(prevPower[ch].empty())
							{
								currPower.at(ch).push_back(log(p));
							}
							else
							{
								float wAvg = ((9 * prevPower[ch][n]) + log(p)) / 10;
								currPower.at(ch).push_back(wAvg);
							}
						}
					}

					if(maxpower > 0);
					{
						XYRange pltRange;
						plt.getRange(pltRange);

						float logPower = log(maxpower);

						if(pltRange.ymax < logPower || (pltRange.ymax - logPower) > 5)
						{
							pltRange.ymax = logPower;
							plt.setRange(pltRange);
						}
					}

					plt.plot(xvalues, currPower[ch], chanColors[ch], 2.0f);

					prevPower[ch] = currPower[ch];
					currPower[ch].clear();
				}
			}
			else //Spectrogram
			{
				canvasPlot->drawSpectrogram(powerReader->at(0));
			}
		}		
	}
}


void SpectrumCanvas::setDisplayType(DisplayType type)
{
	if(type == SPECTROGRAM)
	{
		plt.setVisible(false);
		canvasPlot->clearSpectrogram();
	}
	else
	{
		plt.clear();
		plt.setVisible(true);
	}
	
	if(CoreServices::getAcquisitionStatus())
	{
		stopCallbacks();
		displayType = type;
		canvasPlot->displayType = type;
		startCallbacks();
	}
	else
	{
		displayType = type;
		canvasPlot->displayType = type;
	}

	resized();
	canvasPlot->repaint();
}


/** CANVAS PLOT - Stores the plot along with it's legend*/

CanvasPlot::CanvasPlot(SpectrumViewer* p)
	: activeChannels({1})
	, processor(p)
	, displayType(POWER_SPECTRUM)
{
	spectrogramImg = std::make_unique<Image>(Image::RGB, 1000, 1000, true);
	setOpaque(true);
}


void CanvasPlot::resized()
{

}

void CanvasPlot::drawSpectrogram(std::vector<float> chanData)
{
	auto imageWidth = spectrogramImg->getWidth() - 1;
    auto imageHeight = spectrogramImg->getHeight();

	// first, shuffle our image leftwards by 1 pixel..
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth, imageHeight);

	// find the range of values produced, so we can scale our rendering to
    // show up the detail clearly
    auto maxLevel = juce::FloatVectorOperations::findMaximum (chanData.data(), chanData.size());
	// juce::FloatVectorOperations::findMaximum()

	for (auto y = 0; y < imageHeight; ++y)
	{
		auto skewedProportionY = 1.0f - (float) y / (float) imageHeight;
		auto dataIndex = (size_t) jlimit (0, (int)chanData.size(), (int) (skewedProportionY * chanData.size()));
		auto level = juce::jmap (chanData[dataIndex], 0.0f, juce::jmax (maxLevel, 1e-5f), 0.0f, 1.0f);
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

		// Draw channel color legend next to the plot
		g.setColour(Colours::grey);

		int left = getWidth() - legendThickness - 10;
		int top = 60;

		g.fillRoundedRectangle( left
							, top
							, legendThickness
							, rowHeight * activeChannels.size()
							, 5.0 );
		
		g.setFont(Font("Fira Code", "SemiBold", 16.0f));
		
		for(int i = 0; i < activeChannels.size(); i++)
		{
			top = (i+1) * rowHeight + 10;

			g.setColour(Colours::black);
			String chan = processor->getChanName(activeChannels[i]);
			g.drawFittedText( chan
							, left + 20
							, top + 10
							, (legendThickness - 20) / 2
							, 30
							, Justification::centredLeft
							, 1);
			
			g.setColour(chanColors.at(i));
			g.fillRect( left + (legendThickness / 2)
					, top + 10
					, (legendThickness - 30) / 2
					, 30);
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
				yTick = String(k*100);
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

void CanvasPlot::clearSpectrogram()
{
	spectrogramImg->clear(spectrogramImg->getBounds());
}