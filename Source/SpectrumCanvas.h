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

#ifndef SPECTRUMCANVAS_H_INCLUDED
#define SPECTRUMCANVAS_H_INCLUDED

#include "AtomicSynchronizer.h"
#include "SpectrumViewer.h"
#include <VisualizerWindowHeaders.h>

#include "../../Source/Processors/Visualization/MatlabLikePlot.h" // relative import, may not work

class VerticalGroupSet : public Component
{
public:
	VerticalGroupSet(Colour backgroundColor = Colours::silver);
	VerticalGroupSet(const String& componentName, Colour backgroundColor = Colours::silver);
	~VerticalGroupSet();

	void addGroup(std::initializer_list<Component*> components);

private:
	Colour bgColor;
	int leftBound;
	int rightBound;
	OwnedArray<DrawableRectangle> groups;
	static const int PADDING = 5;
	static const int CORNER_SIZE = 8;
};

/** 

	Draws the real-time power spectrum

*/
class SpectrumCanvas : public Visualizer
	//, public ComboBox::Listener
	//, public Button::Listener
	//, public Label::Listener
{
public:

	/** Constructor */
	SpectrumCanvas(SpectrumViewer* n);

	/** Destructor */
	~SpectrumCanvas();

	/** Updates component boundaries */
	void resized() override;

	/** Called when tab becomes visible again */
	void refreshState() override;

	/** Updates settings */
	void update() override;

	/** Called instead of repaint to avoid re-painting sub-components*/
	void refresh() override;

	/** Starts animation callbacks */
	void beginAnimation() override;

	/** Stops animation callbacks */
	void endAnimation() override;

	/** Draws the canvas */
	void paint(Graphics& g) override;


private:

	SpectrumViewer* processor;

	DisplayType displayType;

	ScopedPointer<Viewport>  viewport;
	ScopedPointer<Component> canvas;
	juce::Rectangle<int> canvasBounds;

	std::vector<std::vector<std::vector<float>>> power; // channels x 5 x freqs

	Array<int> bufferIndex;

	float freqStep;

	int freqStart;
	int freqEnd;

	ScopedPointer<MatlabLikePlot> plot;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumCanvas);
};

#endif // SPECTRUMCANVAS_H_INCLUDED