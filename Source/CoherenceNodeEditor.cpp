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


#include "CoherenceNodeEditor.h"

/************** editor *************/
CoherenceEditor::CoherenceEditor(CoherenceNode* p)
    : VisualizerEditor(p, 300, true), numChannels(0)
{
    tabText = "Spectrogram";
    processor = p;

    ch1Selection = new ComboBox("CH1 ComboBox");
    ch1Selection->setBounds(60, 40, 75, 22);
    ch1Selection->addListener(this);
    ch1Selection->addItem("-", 1);
    ch1Selection->setSelectedId(1, dontSendNotification);
    addAndMakeVisible(ch1Selection);

    ch1Label = new Label("CH1:", "CH1:");
    ch1Label->setFont(Font("Small Text", 13, Font::plain));
    ch1Label->setBounds(20, 40, 35, 22);
    ch1Label->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(ch1Label);

    ch2Selection = new ComboBox("CH2 ComboBox");
    ch2Selection->setBounds(60, 70, 75, 22);
    ch2Selection->addListener(this);
    ch2Selection->addItem("-", 1);
    ch2Selection->setSelectedId(1, dontSendNotification);
    addAndMakeVisible(ch2Selection);

    ch2Label = new Label("CH2:", "CH2:");
    ch2Label->setFont(Font("Small Text", 13, Font::plain));
    ch2Label->setBounds(20, 70, 35, 22);
    ch2Label->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(ch2Label);

    desiredWidth = 250;
    
}

CoherenceEditor::~CoherenceEditor() {}


void CoherenceEditor::comboBoxChanged(ComboBox* comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == ch1Selection)
    {
        getProcessor()->setParameter(0, ch1Selection->getSelectedId() - 2);
    }
}

void CoherenceEditor::updateSettings()
{

    int newChannelCount = getProcessor()->getNumInputs();

    if (newChannelCount != numChannels)
    {

        ch1Selection->clear();

        ch1Selection->addItem("-", 1);

        for (int i = 0; i < newChannelCount; i++)
            ch1Selection->addItem(String(i+1), i+2);

        if (newChannelCount > 0)
            ch1Selection->setSelectedId(2, dontSendNotification);
        else
            ch1Selection->setSelectedId(1, dontSendNotification);

    }
}

void CoherenceEditor::startAcquisition()
{
    canvas->beginAnimation();
}

void CoherenceEditor::stopAcquisition()
{
    canvas->endAnimation();
}

Visualizer* CoherenceEditor::createNewCanvas()
{
    canvas = new CoherenceVisualizer(processor);
    return canvas;
}
