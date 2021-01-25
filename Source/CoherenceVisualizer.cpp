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

#include "CoherenceVisualizer.h"
#include <math.h>
#include <climits>

CoherenceVisualizer::CoherenceVisualizer(CoherenceNode* n)
	: viewport(new Viewport())
	, canvas(new Component("canvas"))
	, processor(n)
	, freqStart(processor->tfrParams.freqStart)
	, freqEnd(processor->tfrParams.freqEnd)
	, canvasBounds(0, 0, 1, 1)
	, displayType(POWER_SPECTRUM)
{
	refreshRate = 10;

	juce::Rectangle<int> bounds;
	
	plot = new MatlabLikePlot();
	plot->setBounds(bounds = { 20, 20, 800, 600 });
	plot->setTitle("POWER SPECTRUM");
	plot->setRange(0, 500, 0.0, 10, false);
	plot->setControlButtonsVisibile(false);
	plot->setAutoRescale(true);
	//plot->drawComponent->setScaleString("Hz", "Power"); // drawComponent is private
	canvas->addAndMakeVisible(plot);

	canvasBounds = canvasBounds.getUnion(bounds);
	canvasBounds.setBottom(canvasBounds.getBottom() + 10);
	canvasBounds.setRight(canvasBounds.getRight() + 10);
	canvas->setBounds(canvasBounds);

	viewport->setViewedComponent(canvas, false);
	viewport->setScrollBarsShown(true, true);
	addAndMakeVisible(viewport);

	power.resize(2);

	for (int ch = 0; ch < 2; ch++)
	{
		power[ch].resize(5);

		for (int i = 0; i < 5; i++)
			power[ch][i].assign(250, 1.0f);
	}
	

	startCallbacks();
}


CoherenceVisualizer::~CoherenceVisualizer()
{
	stopCallbacks();
}

void CoherenceVisualizer::resized()
{
	viewport->setSize(getWidth(), getHeight());
}

void CoherenceVisualizer::refreshState() {}

void CoherenceVisualizer::update()
{
	
}

void CoherenceVisualizer::paint(Graphics& g)
{
	g.fillAll(Colours::lightgrey); // roughly matches editor background (without gradient)
}

void CoherenceVisualizer::refresh()
{
	
	// Update plot if frequency has changed.
	/*if (freqStart != processor->tfrParams.freqStart || freqEnd != processor->tfrParams.freqEnd)
	{
		freqStart = processor->tfrParams.freqStart;
		freqEnd = processor->tfrParams.freqEnd;

		plot ->setRange(processor->tfrParams.freqStart, processor->tfrParams.freqEnd, 0.0, 100, true);
	}*/

	freqStep = processor->tfrParams.freqStep;

	// Get data from processor thread, then plot
	if (displayType == COHERENCE && processor->coherence.hasUpdate())
	{

		/*AtomicScopedReadPtr<std::vector<double>> coherenceReader(processor->coherence);
		coherenceReader.pullUpdate();

		double coh[coherenceReader->at(0).size()];
		disp
		coh[i] = coherenceReader->at(comb)[i] * 100;

		coh.resize();

		line = XYline(freqStart, freqStep, averageCoh, 1, Colours::yellow);

		plot->clearplot();
		plot->plotxy(coherenceLine);
		plot->repaint();*/
	}
	else if (displayType == POWER_SPECTRUM && processor->power.hasUpdate())
	{
		//std::cout << "Reading power data" << std::endl;

		AtomicScopedReadPtr<std::vector<std::vector<float>>> powerReader(processor->power);

		powerReader.pullUpdate();

		//std::cout << "Num freqs: " << powerReader->at(0).size() << std::endl;

		if (powerReader.isValid())
		{
			plot->clearplot();

			for (int ch = 0; ch < 2; ch++)
			{

				float maxpower = -1;
				int maxind = -1;

				for (int n = 0; n < 249; n++)
				{
					float p = powerReader->at(ch)[n];

					if (p > maxpower)
					{
						maxpower = p;
						maxind = n;
					}

					if (p > 0)
						power.at(ch).at(bufferIndex[ch])[n] = log(p);
				}

				//std::cout << "Max ind: " << maxind << std::endl;

				for (int i = 0; i < 5; i++)
				{

					int startIndex = bufferIndex[ch];
					int trueIndex = (startIndex + i) % 5;
					float alphaValue = 1.0f - i * 0.2f;

					Colour color;

					if (ch == 0)
						color = Colours::yellow.withAlpha(alphaValue);
					else
						color = Colours::lightgreen.withAlpha(alphaValue);

					XYline line = XYline(4, 4, power[ch][trueIndex], 1, color);

					plot->plotxy(line);
				}

				bufferIndex.set(ch, (bufferIndex[ch] + 1) % 5);

			}	
		}

		plot->repaint();
		
	}
}

void CoherenceVisualizer::beginAnimation()
{
	
}
void CoherenceVisualizer::endAnimation()
{
	
}


/************ VerticalGroupSet ****************/

VerticalGroupSet::VerticalGroupSet(Colour backgroundColor)
	: Component()
	, bgColor(backgroundColor)
	, leftBound(INT_MAX)
	, rightBound(INT_MIN)
{}

VerticalGroupSet::VerticalGroupSet(const String& componentName, Colour backgroundColor)
	: Component(componentName)
	, bgColor(backgroundColor)
	, leftBound(INT_MAX)
	, rightBound(INT_MIN)
{}

VerticalGroupSet::~VerticalGroupSet() {}

void VerticalGroupSet::addGroup(std::initializer_list<Component*> components)
{
	if (!getParentComponent())
	{
		jassertfalse;
		return;
	}

	DrawableRectangle* thisGroup;
	groups.add(thisGroup = new DrawableRectangle);
	addChildComponent(thisGroup);
	const RelativePoint cornerSize(CORNER_SIZE, CORNER_SIZE);
	thisGroup->setCornerSize(cornerSize);
	thisGroup->setFill(bgColor);

	int topBound = INT_MAX;
	int bottomBound = INT_MIN;
	for (auto component : components)
	{
		Component* componentParent = component->getParentComponent();
		if (!componentParent)
		{
			jassertfalse;
			return;
		}
		int width = component->getWidth();
		int height = component->getHeight();
		Point<int> positionFromItsParent = component->getPosition();
		Point<int> localPosition = getLocalPoint(componentParent, positionFromItsParent);

		// update bounds
		leftBound = jmin(leftBound, localPosition.x - PADDING);
		rightBound = jmax(rightBound, localPosition.x + width + PADDING);
		topBound = jmin(topBound, localPosition.y - PADDING);
		bottomBound = jmax(bottomBound, localPosition.y + height + PADDING);
	}

	// set new background's bounds
	auto bounds = juce::Rectangle<float>::leftTopRightBottom(leftBound, topBound, rightBound, bottomBound);
	thisGroup->setRectangle(bounds);
	thisGroup->setVisible(true);

	// update all other children
	for (DrawableRectangle* group : groups)
	{
		if (group == thisGroup) { continue; }

		topBound = group->getPosition().y;
		bottomBound = topBound + group->getHeight();
		bounds = juce::Rectangle<float>::leftTopRightBottom(leftBound, topBound, rightBound, bottomBound);
		group->setRectangle(bounds);
	}
}
