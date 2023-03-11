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


#include "SpectrumViewerEditor.h"

#include "SpectrumViewer.h"
#include "SpectrumCanvas.h"


SpectrumViewerEditor::SpectrumViewerEditor(GenericProcessor* p)
    : VisualizerEditor(p, "Power Spectrum", 240),
      activeStream(0)
{
	streamSelection = std::make_unique<ComboBox>("Display Stream");
    streamSelection->setBounds(15, 50, 100, 20);
    streamSelection->addListener(this);
    addAndMakeVisible(streamSelection.get());

	streamLabel = std::make_unique<Label>("StreamNameLabel", "Stream");
	streamLabel->setFont(Font("Silkscreen", "Regular", 12));
	streamLabel->setColour(Label::textColourId, Colours::darkgrey);
	streamLabel->setSize(100, 20);
	streamLabel->attachToComponent(streamSelection.get(), false);
	addAndMakeVisible(streamLabel.get());

    addSelectedChannelsParameterEditor("Channels", 15, 90);

	displayType = std::make_unique<ComboBox>("Display Stream");
    displayType->setBounds(125, 50, 110, 20);
    displayType->addListener(this);
	displayType->addItemList({"Power Spectrum", "Spectrogram"}, 1);
	displayType->setSelectedId(1, dontSendNotification);
    addAndMakeVisible(displayType.get());

	displayLabel = std::make_unique<Label>("DisplayTypeLabel", "Display");
	displayLabel->setFont(Font("Silkscreen", "Regular", 12));
	displayLabel->setColour(Label::textColourId, Colours::darkgrey);
	displayLabel->setSize(100, 20);
	displayLabel->attachToComponent(displayType.get(), false);
	addAndMakeVisible(displayLabel.get());
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
    return new SpectrumCanvas((SpectrumViewer*)getProcessor());
}


void SpectrumViewerEditor::startAcquisition()
{
    streamSelection->setEnabled(false);
	streamSelector->setEnabled(false);
	enable();
}


void SpectrumViewerEditor::stopAcquisition()
{
    streamSelection->setEnabled(true);
	streamSelector->setEnabled(true);
	disable();
}

void SpectrumViewerEditor::updateSettings()
{
 
	streamSelection->clear();

	for (auto stream : getProcessor()->getDataStreams())
	{
		if (activeStream == 0)
			activeStream = stream->getStreamId();
		
		streamSelection->addItem(stream->getName(), stream->getStreamId());
	}

	if (streamSelection->indexOfItemId(activeStream) == -1)
	{
		if (streamSelection->getNumItems() > 0)
			activeStream = streamSelection->getItemId(0);
		else
			activeStream = 0;
	}
		
	if (activeStream > 0)
	{
		streamSelection->setSelectedId(activeStream, sendNotification);
	}
}

void SpectrumViewerEditor::comboBoxChanged(ComboBox* cb)
{
	if (cb == streamSelection.get())
	{
		activeStream = cb->getSelectedId();
		
		if (activeStream > 0)
		{
			getProcessor()->getParameter("active_stream")->setNextValue(activeStream);
			streamSelector->setViewedIndex(cb->getSelectedItemIndex());

			for(auto ped : parameterEditors)
			{
				if(ped->getParameterName().equalsIgnoreCase("Channels"))
				{
					auto sp = getProcessor()->getDataStream(activeStream)->getParameter("Channels");
					ped->setParameter(sp);
					sp->setNextValue(sp->getValue());
				}
			}
		}
	}
	else if (cb == displayType.get())
	{
		auto sc = static_cast<SpectrumCanvas*>(canvas.get());

		auto type = (DisplayType)displayType->getSelectedId();

		sc->setDisplayType(type);

		if(type == POWER_SPECTRUM)
			tabText = "Power Spectrum";
		else
			tabText = "Spectrogram";

		if(tabIndex != -1)
		{
			removeTab(tabIndex);
			addTab(tabText, canvas.get());
		}
		else if(dataWindow->isVisible())
		{
			dataWindow->setName(tabText);
		}
	}
}

void SpectrumViewerEditor::selectedStreamHasChanged()
 {
 	if (streamSelection->indexOfItemId(getCurrentStream()) != -1)
 	{
 		activeStream = getCurrentStream();
 		streamSelection->setSelectedId(activeStream, sendNotification);
 	}
 }