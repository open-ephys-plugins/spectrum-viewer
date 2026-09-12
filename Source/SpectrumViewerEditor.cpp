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
    : VisualizerEditor (p, "Power Spectrum", 230)
{
    // Stream and channel selection change the processor's input route, so they
    // belong in the signal chain. Every display control lives on the canvas.
    addSelectedStreamParameterEditor (Parameter::PROCESSOR_SCOPE, "active_stream", 15, 28);
    getParameterEditor ("active_stream")->setBounds (15, 28, 210, 18);

    addSelectedChannelsParameterEditor (Parameter::STREAM_SCOPE, "Channels", 15, 53);
    getParameterEditor ("Channels")->setBounds (15, 53, 210, 18);

    readinessLabel = std::make_unique<Label> ("AnalysisReadiness", "Stopped");
    readinessLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    readinessLabel->setBounds (15, 78, 210, 18);
    addAndMakeVisible (readinessLabel.get());

    startTimerHz (4);
}

SpectrumViewerEditor::~SpectrumViewerEditor()
{
    stopTimer();

    // The canvas writes into displaySettings, which is a member of this class
    // and is therefore destroyed before the base class's canvas pointer.
    // Destroy the canvas first so it can never outlive what it writes to.
    canvas.reset();
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
    // The canvas reads its initial state from displaySettings and writes every
    // change back, so there is nothing to push into it here.
    return new SpectrumCanvas (static_cast<SpectrumViewer*> (getProcessor()),
                               displaySettings);
}

void SpectrumViewerEditor::startAcquisition()
{
    enable();
}

void SpectrumViewerEditor::stopAcquisition()
{
    disable();
}

void SpectrumViewerEditor::timerCallback()
{
    const auto* processor = static_cast<SpectrumViewer*> (getProcessor());

    String text;
    switch (processor->getAnalysisReadiness())
    {
        case SpectrumAnalysisReadiness::preparing:
            text = "Preparing analysis...";
            break;
        case SpectrumAnalysisReadiness::warmingUp:
            text = "Warming up " + String (processor->getWarmupSampleCount()) + "/"
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
        case SpectrumAnalysisReadiness::invalidSelection:
            text = "Select a stream and channels";
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
    // The canvas owns the frequency-range control, so it refreshes the Nyquist
    // entry itself from the processor's active stream.
    if (auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get()))
        spectrumCanvas->updateSettings();
}

void SpectrumViewerEditor::saveVisualizerEditorParameters (XmlElement* xml)
{
    // Serialize the values, never the controls: the canvas may never have been
    // created, and its controls would then not exist to read.
    displaySettings.writeTo (*xml);
}

void SpectrumViewerEditor::loadVisualizerEditorParameters (XmlElement* xml)
{
    displaySettings.readFrom (*xml);

    // Loading can happen after the visualizer has been opened, so an existing
    // canvas has to pick the values up.
    if (auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get()))
        spectrumCanvas->applyDisplaySettings();
}
