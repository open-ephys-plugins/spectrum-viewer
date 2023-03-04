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
	: viewport(new Viewport())
	, canvas(new Component("canvas"))
	, processor(n)
	, canvasBounds(0, 0, 1, 1)
	, numActiveChans(2)
{
	refreshRate = 60;

	juce::Rectangle<int> bounds;
	
	plt.setBounds(bounds = { 20, 20, 1200, 800 });
	plt.title("POWER SPECTRUM");
	XYRange range{ 0, 1000, 0, 20 };
	plt.setRange(range);
	plt.xlabel("Frequency (Hz)");
	plt.ylabel("");

	canvas->addAndMakeVisible(plt);

	canvasBounds = canvasBounds.getUnion(bounds);
	canvasBounds.setBottom(canvasBounds.getBottom() + 10);
	canvasBounds.setRight(canvasBounds.getRight() + 10);
	canvas->setBounds(canvasBounds);

	viewport->setViewedComponent(canvas, false);
	viewport->setScrollBarsShown(true, true);
	addAndMakeVisible(viewport);

	currPower.resize(2);
	prevPower.resize(2);

	nFreqs = processor->tfrParams.nFreqs;
	freqStep = processor->tfrParams.freqStep;

	for (int ch = 0; ch < 2; ch++)
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
	viewport->setSize(getWidth(), getHeight());
}

void SpectrumCanvas::refreshState() {}

void SpectrumCanvas::paint(Graphics& g)
{
	g.fillAll(Colour(28,28,28));
}

void SpectrumCanvas::update()
{
	numActiveChans = processor->getNumActiveChans();
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

			for (int ch = 0; ch < numActiveChans; ch++)
			{
				// LOGC("********** Buffer Index: ", bufferIndex[ch]);
				
				float maxpower = -1;
				int maxind = -1;

				for (int n = 0; n < nFreqs; n++)
				{
					float p = powerReader->at(ch)[n];

					if (p > maxpower)
					{
						maxpower = p;
						maxind = n;
					}

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
				
				XYRange pltRange;
				plt.getRange(pltRange);
				if(pltRange.ymax < log(maxpower))
				{
					pltRange.ymax = log(maxpower);
					plt.setRange(pltRange);
				}
				//std::cout << "Max ind: " << maxind << std::endl;

				Colour chanColor;

				if (ch == 0)
					chanColor = Colours::yellow;
				else
					chanColor = Colours::lightgreen;

				plt.plot(xvalues, currPower[ch], chanColor, 1.5f);

				prevPower[ch] = currPower[ch];
				currPower[ch].clear();

				// for (int i = 0; i < 5; i++)
				// {

				// 	int startIndex = bufferIndex[ch];
				// 	int trueIndex = negativeAwareModulo((startIndex - i), 5);
				// 	float opacity = 1.0f - i * 0.2f;
					
				// 	// LOGC("********** True Index: ", trueIndex, ", Alpha Val: ", alphaValue);

				// 	Colour color;

				// 	if (ch == 0)
				// 		color = Colours::yellow;
				// 	else
				// 		color = Colours::lightgreen;

				// 	plt.plot(xvalues, power[ch][trueIndex], color, 1.5f, opacity);
				// }
			}	
		}

		plt.show();
		
	}
}
