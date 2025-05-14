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

#include "SpectrumCanvas.h"
#include "SpectrumViewer.h"

SpectrumViewerEditor::SpectrumViewerEditor (GenericProcessor* p)
    : VisualizerEditor (p, "Power Spectrum", 220)
{
    addSelectedStreamParameterEditor (Parameter::PROCESSOR_SCOPE, "active_stream", 15, 28);
    getParameterEditor ("active_stream")->setSize (210, 18);

    addSelectedChannelsParameterEditor (Parameter::STREAM_SCOPE, "Channels", 15, 53);
    getParameterEditor ("Channels")->setSize (210, 18);

    displayType = std::make_unique<ComboBox> ("Display Type");
    displayType->setBounds (15, 78, 100, 18);
    displayType->addListener (this);
    displayType->addItemList ({ "Power Spectrum", "Spectrogram" }, 1);
    displayType->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (displayType.get());

    displayLabel = std::make_unique<Label> ("DisplayTypeLabel", "Display");
    displayLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    displayLabel->setBounds (123, 78, 80, 18);
    addAndMakeVisible (displayLabel.get());

    freqRanges.add (Range (0, 100));
    freqRanges.add (Range (0, 500));
    freqRanges.add (Range (0, 1000));
    frequencyRange = std::make_unique<ComboBox> ("FreqRange");
    frequencyRange->setBounds (15, 103, 100, 18);
    frequencyRange->addListener (this);
    frequencyRange->addItemList ({ "0 - 100", "0 - 500", "0 - 1000" }, 1);
    frequencyRange->setSelectedId (3, dontSendNotification);
    addAndMakeVisible (frequencyRange.get());

    frequencyLabel = std::make_unique<Label> ("FreqRangeLabel", "Freq. Range");
    frequencyLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    frequencyLabel->setBounds (123, 103, 80, 18);
    addAndMakeVisible (frequencyLabel.get());
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
    // Create a new canvas and pass the processor ptr
    auto sp = (SpectrumViewer*) getProcessor();
    auto spectrumCanvas = new SpectrumCanvas (sp);

    // Set frequency range for canvas
    Range<int> range = freqRanges[frequencyRange->getSelectedItemIndex()];
    spectrumCanvas->getPlotPtr()->setFrequencyRange (range.getStart(), range.getEnd(), sp->getFreqStep());

    // Set display type for canvas
    auto type = (DisplayType) displayType->getSelectedId();
    spectrumCanvas->setDisplayType (type);

    return spectrumCanvas;
}

void SpectrumViewerEditor::startAcquisition()
{
    frequencyRange->setEnabled (false);
    enable();
}

void SpectrumViewerEditor::stopAcquisition()
{
    frequencyRange->setEnabled (true);
    disable();
}

void SpectrumViewerEditor::comboBoxChanged (ComboBox* cb)
{
    auto sc = static_cast<SpectrumCanvas*> (canvas.get());

    if (cb == displayType.get())
    {
        auto type = (DisplayType) displayType->getSelectedId();

        if (! sc)
            return;

        sc->setDisplayType (type);
    }
    else if (cb == frequencyRange.get())
    {
        Range<int> range = freqRanges[cb->getSelectedItemIndex()];

        // Send frequency range update to processor
        auto processor = static_cast<SpectrumViewer*> (getProcessor());
        processor->setFrequencyRange (range);

        // Send frequency range update to canvas plot
        if (sc != nullptr)
        {
            sc->getPlotPtr()->setFrequencyRange (range.getStart(),
                                                 range.getEnd(),
                                                 processor->getFreqStep());
        }
    }
}

void SpectrumViewerEditor::selectedStreamHasChanged()
{
    if (getProcessor()->getDataStreams().size() > 0)
    {
        auto stream = getProcessor()->getDataStream (getCurrentStream());
        // Add or change the currently selected stream's max frequency
        float maxFreq = stream->getSampleRate() / 2;

        freqRanges.set (3, Range (0, (int) maxFreq));

        if (frequencyRange->getNumItems() == 4)
        {
            int selectedId = frequencyRange->getSelectedId();
            frequencyRange->changeItemText (4, "0 - " + String (maxFreq));

            if (selectedId == 4)
            {
                frequencyRange->setText ("0 - " + String (maxFreq), sendNotification);
            }
        }
        else
            frequencyRange->addItem ("0 - " + String (maxFreq), 4);
    }
}

void SpectrumViewerEditor::saveVisualizerEditorParameters (XmlElement* xml)
{
    xml->setAttribute ("display_type", displayType->getSelectedId());
    xml->setAttribute ("frequency_range", frequencyRange->getSelectedId());
}

void SpectrumViewerEditor::loadVisualizerEditorParameters (XmlElement* xml)
{
    int selectedType = xml->getIntAttribute ("display_type", 1);
    displayType->setSelectedId (selectedType, sendNotification);

    int selectedRange = xml->getIntAttribute ("frequency_range", 3);
    frequencyRange->setSelectedId (selectedRange, sendNotification);
}