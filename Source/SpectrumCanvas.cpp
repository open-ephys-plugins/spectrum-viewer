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

void SpectrumCanvas::refreshState() {}

void SpectrumCanvas::paint(Graphics& g)
{
	g.fillAll(Colour(28,28,28));
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
	}
}



CanvasPlot::CanvasPlot(SpectrumViewer* p)
	: activeChannels({1})
	, processor(p)
{

}


void CanvasPlot::resized()
{

}

void CanvasPlot::paint(Graphics& g)
{
	g.fillAll(Colour(28,28,28));

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
						, left + 10
						, top + 10
						, (legendThickness - 20) / 2
						, 30
						, Justification::centredLeft
						, 1);
		
		g.setColour(chanColors.at(i));
		g.fillRect( left + (legendThickness / 2)
				  , top + 10
				  , (legendThickness - 20) / 2
				  , 30);
	}
}