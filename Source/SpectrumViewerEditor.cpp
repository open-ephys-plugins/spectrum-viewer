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
    : VisualizerEditor (p, "Power Spectrum", 270)
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
    freqRanges.add (Range (0, 1000)); // Updated to the selected stream's Nyquist.
    frequencyRange = std::make_unique<ComboBox> ("FreqRange");
    frequencyRange->setBounds (15, 103, 100, 18);
    frequencyRange->addListener (this);
    frequencyRange->addItemList ({ "0 - 100", "0 - 500", "0 - 1000", "Full" }, 1);
    frequencyRange->setSelectedId (4, dontSendNotification);
    addAndMakeVisible (frequencyRange.get());

    frequencyLabel = std::make_unique<Label> ("FreqRangeLabel", "Freq. Range");
    frequencyLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    frequencyLabel->setBounds (123, 103, 80, 18);
    addAndMakeVisible (frequencyLabel.get());

    analysisProfile = std::make_unique<ComboBox> ("AnalysisProfile");
    analysisProfile->setBounds (15, 128, 100, 18);
    analysisProfile->addListener (this);
    analysisProfile->addItemList ({ "Fast", "Balanced", "Fine" }, 1);
    analysisProfile->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (analysisProfile.get());

    profileLabel = std::make_unique<Label> ("AnalysisProfileLabel", "Analysis");
    profileLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    profileLabel->setBounds (123, 128, 80, 18);
    addAndMakeVisible (profileLabel.get());

    frequencyScale = std::make_unique<ComboBox> ("FrequencyScale");
    frequencyScale->setBounds (15, 153, 100, 18);
    frequencyScale->addListener (this);
    frequencyScale->addItemList ({ "Linear", "Log" }, 1);
    frequencyScale->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (frequencyScale.get());

    scaleLabel = std::make_unique<Label> ("FrequencyScaleLabel", "Frequency Axis");
    scaleLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    scaleLabel->setBounds (123, 153, 95, 18);
    addAndMakeVisible (scaleLabel.get());

    amplitudeDisplay = std::make_unique<ComboBox> ("AmplitudeDisplay");
    amplitudeDisplay->setBounds (15, 178, 100, 18);
    amplitudeDisplay->addListener (this);
    amplitudeDisplay->addItemList ({ "PSD", "ASD" }, 1);
    amplitudeDisplay->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (amplitudeDisplay.get());

    amplitudeLabel = std::make_unique<Label> ("AmplitudeDisplayLabel", "Values");
    amplitudeLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    amplitudeLabel->setBounds (123, 178, 80, 18);
    addAndMakeVisible (amplitudeLabel.get());

    readinessLabel = std::make_unique<Label> ("AnalysisReadiness", "Stopped");
    readinessLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    readinessLabel->setBounds (15, 203, 220, 18);
    addAndMakeVisible (readinessLabel.get());
    startTimerHz (4);
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
    spectrumCanvas->getPlotPtr()->setAmplitudeDisplay (
        static_cast<SpectrumAmplitudeDisplay> (amplitudeDisplay->getSelectedId()));

    return spectrumCanvas;
}

void SpectrumViewerEditor::startAcquisition()
{
    enable();
}

void SpectrumViewerEditor::stopAcquisition()
{
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
        if (cb->getSelectedId() == 4)
            processor->setFullFrequencyRange();
        else
            processor->setFrequencyRange (range);

        // Send frequency range update to canvas plot
        if (sc != nullptr)
        {
            sc->getPlotPtr()->setFrequencyRange (range.getStart(),
                                                 range.getEnd(),
                                                 processor->getFreqStep());
        }
    }
    else if (cb == analysisProfile.get())
    {
        auto processor = static_cast<SpectrumViewer*> (getProcessor());
        processor->setAnalysisProfile (
            static_cast<SpectrumAnalysisProfile> (analysisProfile->getSelectedId()));
    }
    else if (cb == frequencyScale.get())
    {
        static_cast<SpectrumViewer*> (getProcessor())->setFrequencyScale (cb->getSelectedId() == 2 ? spectrumviewer::FrequencyScale::logarithmic : spectrumviewer::FrequencyScale::linear);
    }
    else if (cb == amplitudeDisplay.get() && sc != nullptr)
    {
        sc->getPlotPtr()->setAmplitudeDisplay (
            static_cast<SpectrumAmplitudeDisplay> (cb->getSelectedId()));
    }
}

void SpectrumViewerEditor::timerCallback()
{
    auto processor = static_cast<SpectrumViewer*> (getProcessor());
    String text;
    switch (processor->getAnalysisReadiness())
    {
        case SpectrumAnalysisReadiness::preparing:
            text = "Preparing analysis...";
            break;
        case SpectrumAnalysisReadiness::warmingUp:
            text = "Warming up "
                   + String (processor->getWarmupSampleCount()) + "/"
                   + String (processor->getWarmupTargetSampleCount());
            break;
        case SpectrumAnalysisReadiness::live:
            text = processor->isAnalysisConfigurationPending()
                       ? "Live (preparing new profile...)"
                       : "Live";
            break;
        case SpectrumAnalysisReadiness::configurationFailed:
            text = processor->hasActiveAnalysis()
                       ? "Live (new profile failed)"
                       : "Analysis configuration failed";
            break;
        case SpectrumAnalysisReadiness::stopped:
        default:
            text = "Stopped";
            break;
    }
    readinessLabel->setText (text, dontSendNotification);
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
            frequencyRange->changeItemText (4, "Full (0 - " + String (maxFreq) + ")");

            if (selectedId == 4)
            {
                frequencyRange->setText ("Full (0 - " + String (maxFreq) + ")", sendNotification);
            }
        }
        else
            frequencyRange->addItem ("Full (0 - " + String (maxFreq) + ")", 4);

        if (frequencyRange->getSelectedId() == 4)
            static_cast<SpectrumViewer*> (getProcessor())->setFullFrequencyRange();
    }
}

void SpectrumViewerEditor::saveVisualizerEditorParameters (XmlElement* xml)
{
    xml->setAttribute ("display_type", displayType->getSelectedId());
    xml->setAttribute ("frequency_range", frequencyRange->getSelectedId());
    xml->setAttribute ("analysis_profile", analysisProfile->getSelectedId());
    xml->setAttribute ("frequency_scale", frequencyScale->getSelectedId());
    xml->setAttribute ("amplitude_display", amplitudeDisplay->getSelectedId());
}

void SpectrumViewerEditor::loadVisualizerEditorParameters (XmlElement* xml)
{
    int selectedType = xml->getIntAttribute ("display_type", 1);
    displayType->setSelectedId (selectedType, sendNotification);

    int selectedRange = xml->getIntAttribute ("frequency_range", 4);
    frequencyRange->setSelectedId (selectedRange, sendNotification);

    int selectedProfile = xml->getIntAttribute ("analysis_profile", 1);
    analysisProfile->setSelectedId (selectedProfile, sendNotification);

    frequencyScale->setSelectedId (
        xml->getIntAttribute ("frequency_scale", 1), sendNotification);
    amplitudeDisplay->setSelectedId (
        xml->getIntAttribute ("amplitude_display", 1), sendNotification);
}
