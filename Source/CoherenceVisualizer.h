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

#ifndef COHERENCE_VIS_H_INCLUDED
#define COHERENCE_VIS_H_INCLUDED

#include "AtomicSynchronizer.h"
#include "CoherenceNode.h"
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

class CoherenceVisualizer : public Visualizer
	//, public ComboBox::Listener
	//, public Button::Listener
	//, public Label::Listener
{
public:
	CoherenceVisualizer(CoherenceNode* n);
	~CoherenceVisualizer();

	void resized() override;

	void refreshState() override;
	void update() override;
	void refresh() override;
	void beginAnimation() override;
	void endAnimation() override;
	//void setParameter(int, float) override;
	//void setParameter(int, int, int, float) override;
	//void comboBoxChanged(ComboBox* comboBoxThatHasChanged) override;
	//void labelTextChanged(Label* labelThatHasChanged) override;
	//void buttonEvent(Button* buttonEvent);
	//void buttonClicked(Button* buttonClick) override;
	void paint(Graphics& g) override;

	void setParameter(int, float) {}
	void setParameter(int, int, int, float) {}

private:

	CoherenceNode* processor;

	DisplayType displayType;

	ScopedPointer<Viewport>  viewport;
	ScopedPointer<Component> canvas;
	juce::Rectangle<int> canvasBounds;

	std::vector<std::vector<std::vector<float>>> power; // channels x 10 x freqs

	Array<int> bufferIndex;

	float freqStep;

	int freqStart;
	int freqEnd;

	ScopedPointer<MatlabLikePlot> plot;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CoherenceVisualizer);
};

#endif // COHERENCE_VIS_H_INCLUDED