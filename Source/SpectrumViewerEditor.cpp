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
	: VisualizerEditor(p, "Power Spectrum", 240)
	, activeStream(0)
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

	freqRanges.add(Range(0, 100));
	freqRanges.add(Range(0, 500));
	freqRanges.add(Range(0, 1000));
	frequencyRange = std::make_unique<ComboBox>("FreqRange");
	frequencyRange->setBounds(125, 95, 110, 20);
	frequencyRange->addListener(this);
	frequencyRange->addItemList({"0 - 100", "0 - 500", "0 - 1000"}, 1);
	frequencyRange->setSelectedId(3, dontSendNotification);
	addAndMakeVisible(frequencyRange.get());

	frequencyLabel = std::make_unique<Label>("FreqRangeLabel", "Freq. Range");
	frequencyLabel->setFont(Font("Silkscreen", "Regular", 12));
	frequencyLabel->setColour(Label::textColourId, Colours::darkgrey);
	frequencyLabel->setSize(100, 20);
	frequencyLabel->attachToComponent(frequencyRange.get(), false);
	addAndMakeVisible(frequencyLabel.get());
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
	// Create a new canvas and pass the processor ptr
	auto sp = (SpectrumViewer*)getProcessor();
	auto spectrumCanvas = new SpectrumCanvas(sp);

	// Set frequency range for canvas
	Range<int> range = freqRanges[frequencyRange->getSelectedItemIndex()];
	spectrumCanvas->getPlotPtr()->setFrequencyRange(range.getStart(), range.getEnd(), sp->getFreqStep());

	// Set display type for canvas
	auto type = (DisplayType)displayType->getSelectedId();
	spectrumCanvas->setDisplayType(type);

	return spectrumCanvas;
}


void SpectrumViewerEditor::startAcquisition()
{
	streamSelection->setEnabled(false);
	streamSelector->setEnabled(false);
	frequencyRange->setEnabled(false);
	enable();
}


void SpectrumViewerEditor::stopAcquisition()
{
	streamSelection->setEnabled(true);
	streamSelector->setEnabled(true);
	frequencyRange->setEnabled(true);
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
	auto sc = static_cast<SpectrumCanvas*>(canvas.get());

	if (cb == streamSelection.get())
	{
		activeStream = cb->getSelectedId();
		
		if (activeStream > 0)
		{
			auto stream = getProcessor()->getDataStream(activeStream);
			// Add or change the currently selected stream's max frequency
			float maxFreq = stream->getSampleRate() / 2;

			freqRanges.set(3, Range(0, (int)maxFreq));
			
			if(frequencyRange->getNumItems() == 4)
			{
				int selectedId = frequencyRange->getSelectedId();
				frequencyRange->changeItemText(4, "0 - " + String(maxFreq));

				if(selectedId == 4)
				{
					frequencyRange->setText("0 - " + String(maxFreq), sendNotification);
				}
			}
			else
				frequencyRange->addItem("0 - " + String(maxFreq), 4);
			

			// send updates for active stream and selected channels
			getProcessor()->getParameter("active_stream")->setNextValue(activeStream);
			streamSelector->setViewedIndex(cb->getSelectedItemIndex());

			for(auto ped : parameterEditors)
			{
				if(ped->getParameterName().equalsIgnoreCase("Channels"))
				{
					auto sp = stream->getParameter("Channels");
					ped->setParameter(sp);
					getProcessor()->parameterChangeRequest(sp);
					break;
				}
			}
			
		}
	}
	else if (cb == displayType.get())
	{

		auto type = (DisplayType)displayType->getSelectedId();

		if(!sc)
			return;

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
		else if(dataWindow)
		{
			dataWindow->setName(tabText);
		}
	}
	else if (cb == frequencyRange.get())
	{
		Range<int> range = freqRanges[cb->getSelectedItemIndex()];
		
		// Send frequency range update to processor
		auto processor = static_cast<SpectrumViewer*>(getProcessor());
		processor->setFrequencyRange(range);

		// Send frequency range update to canvas plot
		if(sc != nullptr)
		{
			sc->getPlotPtr()->setFrequencyRange(range.getStart(),
												range.getEnd(),
												processor->getFreqStep());
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

void SpectrumViewerEditor::saveVisualizerEditorParameters(XmlElement* xml)
{
	xml->setAttribute("display_type", displayType->getSelectedId());
	xml->setAttribute("frequency_range", frequencyRange->getSelectedId());
}

void SpectrumViewerEditor::loadVisualizerEditorParameters(XmlElement* xml)
{
	int selectedType = xml->getIntAttribute("display_type", 1);
	displayType->setSelectedId(selectedType, sendNotification);

	int selectedRange = xml->getIntAttribute("frequency_range", 3);
	frequencyRange->setSelectedId(selectedRange, sendNotification);
}