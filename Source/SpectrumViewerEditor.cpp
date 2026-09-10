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

namespace
{
void applyReadableTextBoxColours (Slider& slider)
{
    const auto text = slider.findColour (ThemeColours::controlPanelText);
    const auto background = slider.findColour (ThemeColours::widgetBackground);
    slider.setColour (Slider::textBoxTextColourId, text);
    slider.setColour (Slider::textBoxBackgroundColourId, background);
    slider.setColour (Slider::textBoxHighlightColourId,
                      text.withAlpha (0.25f));
    for (auto* child : slider.getChildren())
    {
        if (auto* label = dynamic_cast<Label*> (child))
        {
            label->setColour (Label::textWhenEditingColourId, text);
            label->setColour (Label::backgroundWhenEditingColourId, background);
        }
    }
}
} // namespace

SpectrumViewerEditor::SpectrumViewerEditor (GenericProcessor* p)
    : VisualizerEditor (p, "Power Spectrum", 230)
{
    addSelectedStreamParameterEditor (Parameter::PROCESSOR_SCOPE, "active_stream", 15, 28);
    getParameterEditor ("active_stream")->setSize (210, 18);

    addSelectedChannelsParameterEditor (Parameter::STREAM_SCOPE, "Channels", 15, 53);
    getParameterEditor ("Channels")->setSize (210, 18);

    displayType = std::make_unique<ComboBox> ("Display Type");
    displayType->setBounds (330, 28, 100, 18);
    displayType->addListener (this);
    displayType->addItemList ({ "Power Spectrum", "Spectrogram" }, 1);
    displayType->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (displayType.get());

    displayLabel = std::make_unique<Label> ("DisplayTypeLabel", "Display");
    displayLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    displayLabel->setBounds (230, 28, 95, 18);
    addAndMakeVisible (displayLabel.get());

    freqRanges.add (Range (0, 100));
    freqRanges.add (Range (0, 500));
    freqRanges.add (Range (0, 1000));
    freqRanges.add (Range (0, 1000)); // Updated to the selected stream's Nyquist.
    frequencyRange = std::make_unique<ComboBox> ("FreqRange");
    frequencyRange->setBounds (330, 53, 100, 18);
    frequencyRange->addListener (this);
    frequencyRange->addItemList ({ "0 - 100", "0 - 500", "0 - 1000", "Full" }, 1);
    frequencyRange->setSelectedId (4, dontSendNotification);
    addAndMakeVisible (frequencyRange.get());

    frequencyLabel = std::make_unique<Label> ("FreqRangeLabel", "Frequency range");
    frequencyLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    frequencyLabel->setBounds (230, 53, 95, 18);
    addAndMakeVisible (frequencyLabel.get());

    analysisProfile = std::make_unique<ComboBox> ("AnalysisProfile");
    analysisProfile->setBounds (15, 78, 100, 18);
    analysisProfile->addListener (this);
    analysisProfile->addItemList ({ "Fast", "Balanced", "Fine" }, 1);
    analysisProfile->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (analysisProfile.get());

    profileLabel = std::make_unique<Label> ("AnalysisProfileLabel", "Analysis");
    profileLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    profileLabel->setBounds (123, 78, 80, 18);
    addAndMakeVisible (profileLabel.get());

    frequencyScale = std::make_unique<ComboBox> ("FrequencyScale");
    frequencyScale->setBounds (330, 78, 100, 18);
    frequencyScale->addListener (this);
    frequencyScale->addItemList ({ "Linear", "Log" }, 1);
    frequencyScale->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (frequencyScale.get());

    scaleLabel = std::make_unique<Label> ("FrequencyScaleLabel", "Frequency scale");
    scaleLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    scaleLabel->setBounds (230, 78, 95, 18);
    addAndMakeVisible (scaleLabel.get());

    amplitudeDisplay = std::make_unique<ComboBox> ("AmplitudeDisplay");
    amplitudeDisplay->setBounds (330, 103, 100, 18);
    amplitudeDisplay->addListener (this);
    amplitudeDisplay->addItemList ({ "PSD", "ASD" }, 1);
    amplitudeDisplay->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (amplitudeDisplay.get());

    amplitudeLabel = std::make_unique<Label> ("AmplitudeDisplayLabel", "Y units");
    amplitudeLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    amplitudeLabel->setBounds (230, 103, 95, 18);
    addAndMakeVisible (amplitudeLabel.get());

    baselineDisplay = std::make_unique<ComboBox> ("AperiodicDisplay");
    baselineDisplay->addListener (this);
    baselineDisplay->addItemList ({ "Off", "Show fit", "Remove" }, 1);
    baselineDisplay->setSelectedId (1, dontSendNotification);
    baselineDisplay->setTooltip (
        "Show or subtract a robust broad spectral background; the PSD estimate is unchanged");
    addAndMakeVisible (baselineDisplay.get());

    baselineLabel = std::make_unique<Label> ("AperiodicDisplayLabel", "Background");
    baselineLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    addAndMakeVisible (baselineLabel.get());

    peakEnvelope = std::make_unique<ToggleButton> ("Peak envelope");
    peakEnvelope->setToggleState (true, dontSendNotification);
    peakEnvelope->setTooltip (
        "Show the maximum spectral power contributing to each display column");
    peakEnvelope->addListener (this);
    addAndMakeVisible (peakEnvelope.get());

    amplitudeRangeMode = std::make_unique<ComboBox> ("AmplitudeRangeMode");
    amplitudeRangeMode->setBounds (540, 28, 100, 18);
    amplitudeRangeMode->addListener (this);
    amplitudeRangeMode->addItemList ({ "Auto", "Fixed" }, 1);
    amplitudeRangeMode->setSelectedId (1, dontSendNotification);
    addAndMakeVisible (amplitudeRangeMode.get());

    amplitudeRangeLabel = std::make_unique<Label> ("AmplitudeRangeLabel", "dB Range");
    amplitudeRangeLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    amplitudeRangeLabel->setBounds (445, 28, 90, 18);
    addAndMakeVisible (amplitudeRangeLabel.get());

    minimumDb = std::make_unique<Slider> ("MinimumDb");
    minimumDb->setRange (-240.0, 100.0, 1.0);
    minimumDb->setValue (-25.0, dontSendNotification);
    minimumDb->setSliderStyle (Slider::LinearHorizontal);
    minimumDb->setTextBoxStyle (Slider::TextBoxLeft, false, 55, 18);
    minimumDb->setBounds (540, 78, 100, 18);
    applyReadableTextBoxColours (*minimumDb);
    minimumDb->addListener (this);
    addAndMakeVisible (minimumDb.get());

    minimumDbLabel = std::make_unique<Label> ("MinimumDbLabel", "Minimum dB");
    minimumDbLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    minimumDbLabel->setBounds (445, 78, 90, 18);
    addAndMakeVisible (minimumDbLabel.get());

    maximumDb = std::make_unique<Slider> ("MaximumDb");
    maximumDb->setRange (-220.0, 120.0, 1.0);
    maximumDb->setValue (25.0, dontSendNotification);
    maximumDb->setSliderStyle (Slider::LinearHorizontal);
    maximumDb->setTextBoxStyle (Slider::TextBoxLeft, false, 55, 18);
    maximumDb->setBounds (540, 53, 100, 18);
    applyReadableTextBoxColours (*maximumDb);
    maximumDb->addListener (this);
    addAndMakeVisible (maximumDb.get());

    maximumDbLabel = std::make_unique<Label> ("MaximumDbLabel", "Maximum dB");
    maximumDbLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    maximumDbLabel->setBounds (445, 53, 90, 18);
    addAndMakeVisible (maximumDbLabel.get());

    automaticRangeLabel = std::make_unique<Label> ("AutomaticRangeLabel", "Awaiting spectrum...");
    automaticRangeLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    automaticRangeLabel->setBounds (540, 53, 115, 18);
    addAndMakeVisible (automaticRangeLabel.get());

    captureDuration = std::make_unique<ComboBox> ("CaptureDuration");
    captureDuration->setBounds (765, 28, 100, 18);
    captureDuration->addItemList ({ "10 s", "30 s", "60 s" }, 1);
    captureDuration->setSelectedId (1, dontSendNotification);
    captureDuration->setTooltip (
        "Averages non-overlapping two-second Fine spectra in linear power");
    addAndMakeVisible (captureDuration.get());

    captureDurationLabel = std::make_unique<Label> ("CaptureDurationLabel", "Capture Length");
    captureDurationLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    captureDurationLabel->setBounds (660, 28, 100, 18);
    addAndMakeVisible (captureDurationLabel.get());

    captureAction = std::make_unique<UtilityButton> ("Capture");
    captureAction->setBounds (660, 53, 100, 20);
    captureAction->addListener (this);
    addAndMakeVisible (captureAction.get());

    captureStatusLabel = std::make_unique<Label> ("CaptureStatus", "Live");
    captureStatusLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    captureStatusLabel->setBounds (768, 53, 100, 20);
    addAndMakeVisible (captureStatusLabel.get());

    readinessLabel = std::make_unique<Label> ("AnalysisReadiness", "Stopped");
    readinessLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    readinessLabel->setBounds (15, 103, 200, 18);
    addAndMakeVisible (readinessLabel.get());

    comparisonMode = std::make_unique<ComboBox> ("SpectrumComparisonMode");
    comparisonMode->setBounds (765, 78, 100, 18);
    comparisonMode->addItemList ({ "Absolute", "Overlay", "Delta" }, 1);
    comparisonMode->setSelectedId (1, dontSendNotification);
    comparisonMode->addListener (this);
    addAndMakeVisible (comparisonMode.get());

    comparisonModeLabel = std::make_unique<Label> ("SpectrumComparisonModeLabel", "Comparison");
    comparisonModeLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    comparisonModeLabel->setBounds (660, 78, 100, 18);
    addAndMakeVisible (comparisonModeLabel.get());

    setReferenceAction = std::make_unique<UtilityButton> ("Set Reference");
    setReferenceAction->setBounds (660, 103, 100, 20);
    setReferenceAction->addListener (this);
    addAndMakeVisible (setReferenceAction.get());

    clearReferenceAction = std::make_unique<UtilityButton> ("Clear Ref");
    clearReferenceAction->setBounds (768, 103, 100, 20);
    clearReferenceAction->addListener (this);
    addAndMakeVisible (clearReferenceAction.get());

    referenceStatusLabel = std::make_unique<Label> ("SpectrumReferenceStatus", "No reference");
    referenceStatusLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    referenceStatusLabel->setBounds (445, 103, 200, 18);
    addAndMakeVisible (referenceStatusLabel.get());

    // Stream and channel selection change the processor route, so keep them in
    // the signal-chain editor. SpectrumCanvas reparents display-only controls.
    getParameterEditor ("active_stream")->setBounds (15, 28, 210, 18);
    getParameterEditor ("Channels")->setBounds (15, 53, 210, 18);
    readinessLabel->setBounds (15, 78, 210, 18);
    Component* canvasControls[] {
        displayType.get(), displayLabel.get(),
        frequencyRange.get(), frequencyLabel.get(),
        analysisProfile.get(), profileLabel.get(),
        frequencyScale.get(), scaleLabel.get(),
        amplitudeDisplay.get(), amplitudeLabel.get(),
        baselineDisplay.get(), baselineLabel.get(),
        peakEnvelope.get(),
        amplitudeRangeMode.get(), amplitudeRangeLabel.get(),
        minimumDb.get(), minimumDbLabel.get(),
        maximumDb.get(), maximumDbLabel.get(), automaticRangeLabel.get(),
        captureDuration.get(), captureDurationLabel.get(),
        captureAction.get(), captureStatusLabel.get(),
        comparisonMode.get(), comparisonModeLabel.get(),
        setReferenceAction.get(), clearReferenceAction.get(),
        referenceStatusLabel.get()
    };
    for (auto* control : canvasControls)
        removeChildComponent (control);

    updateAmplitudeRangeControls();
    startTimerHz (4);
}

SpectrumViewerEditor::~SpectrumViewerEditor()
{
    // Detach canvas children before the editor-owned controls are destroyed.
    canvas.reset();
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
    // Create a new canvas and pass the processor ptr
    auto sp = (SpectrumViewer*) getProcessor();
    auto spectrumCanvas = new SpectrumCanvas (sp, this);

    // Set frequency range for canvas
    Range<int> range = freqRanges[frequencyRange->getSelectedItemIndex()];
    spectrumCanvas->getPlotPtr()->setFrequencyRange (range.getStart(), range.getEnd(), sp->getFreqStep());

    // Set display type for canvas
    auto type = (DisplayType) displayType->getSelectedId();
    spectrumCanvas->setDisplayType (type);
    spectrumCanvas->getPlotPtr()->setAmplitudeDisplay (
        static_cast<SpectrumAmplitudeDisplay> (amplitudeDisplay->getSelectedId()));
    spectrumCanvas->getPlotPtr()->setAperiodicDisplayMode (
        static_cast<spectrumviewer::AperiodicDisplayMode> (
            baselineDisplay->getSelectedId()));
    spectrumCanvas->getPlotPtr()->setPeakEnvelopeVisible (
        peakEnvelope->getToggleState());
    sp->setAperiodicDisplayMode (
        static_cast<spectrumviewer::AperiodicDisplayMode> (
            baselineDisplay->getSelectedId()));
    spectrumCanvas->getPlotPtr()->setFixedAmplitudeRange (
        static_cast<float> (minimumDb->getValue()),
        static_cast<float> (maximumDb->getValue()));
    spectrumCanvas->getPlotPtr()->setAmplitudeRangeMode (
        static_cast<spectrumviewer::AmplitudeRangeMode> (
            amplitudeRangeMode->getSelectedId()));

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
        const auto selectedIndex = cb->getSelectedItemIndex();
        if (! isPositiveAndBelow (selectedIndex, freqRanges.size()))
            return;

        Range<int> range = freqRanges[selectedIndex];

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
    else if (cb == baselineDisplay.get())
    {
        static_cast<SpectrumViewer*> (getProcessor())->setAperiodicDisplayMode (
            static_cast<spectrumviewer::AperiodicDisplayMode> (
                cb->getSelectedId()));
        if (sc != nullptr)
            sc->getPlotPtr()->setAperiodicDisplayMode (
                static_cast<spectrumviewer::AperiodicDisplayMode> (
                    cb->getSelectedId()));
    }
    else if (cb == amplitudeRangeMode.get())
    {
        updateAmplitudeRangeControls();
        applyAmplitudeRangeToCanvas();
    }
    else if (cb == comparisonMode.get())
    {
        static_cast<SpectrumViewer*> (getProcessor())->setSpectrumComparisonMode (
            static_cast<spectrumviewer::SpectrumComparisonMode> (
                comparisonMode->getSelectedId()));
        updateAmplitudeRangeControls();
    }
}

void SpectrumViewerEditor::sliderValueChanged (Slider* slider)
{
    constexpr double minimumSpanDb = 20.0;
    if (slider == minimumDb.get()
        && minimumDb->getValue() > maximumDb->getValue() - minimumSpanDb)
        minimumDb->setValue (maximumDb->getValue() - minimumSpanDb,
                             dontSendNotification);
    else if (slider == maximumDb.get()
             && maximumDb->getValue() < minimumDb->getValue() + minimumSpanDb)
        maximumDb->setValue (minimumDb->getValue() + minimumSpanDb,
                             dontSendNotification);
    applyAmplitudeRangeToCanvas();
}

void SpectrumViewerEditor::buttonClicked (Button* button)
{
    auto* processor = static_cast<SpectrumViewer*> (getProcessor());
    if (button == peakEnvelope.get())
    {
        if (auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get()))
            spectrumCanvas->getPlotPtr()->setPeakEnvelopeVisible (
                peakEnvelope->getToggleState());
        return;
    }
    if (button == setReferenceAction.get())
    {
        if (processor->setCurrentCaptureAsReference())
        {
            // Captures always use Fine analysis. Restore the same estimator
            // when returning live so the reference remains comparable.
            analysisProfile->setSelectedId (
                static_cast<int> (SpectrumAnalysisProfile::fine),
                sendNotification);
            comparisonMode->setSelectedId (
                static_cast<int> (spectrumviewer::SpectrumComparisonMode::overlay),
                sendNotification);
        }
        return;
    }
    if (button == clearReferenceAction.get())
    {
        processor->clearSpectrumReference();
        return;
    }
    if (button != captureAction.get())
        return;

    switch (processor->getCaptureState())
    {
        case SpectrumCaptureState::live:
        case SpectrumCaptureState::failed:
        {
            constexpr double durations[] { 10.0, 30.0, 60.0 };
            const auto index = jlimit (0, 2, captureDuration->getSelectedItemIndex());
            processor->startSpectrumCapture (durations[index]);
            break;
        }
        case SpectrumCaptureState::preparing:
        case SpectrumCaptureState::capturing:
            processor->cancelSpectrumCapture();
            break;
        case SpectrumCaptureState::frozen:
            processor->returnToLive();
            break;
        case SpectrumCaptureState::restoringLive:
            break;
    }
}

void SpectrumViewerEditor::updateAmplitudeRangeControls()
{
    const auto* processor = static_cast<SpectrumViewer*> (getProcessor());
    const auto compatibleReference = processor->hasSpectrumReference()
                                     && processor->getReferenceCompatibility()
                                            == spectrumviewer::SpectrumReferenceCompatibility::compatible;
    const auto comparisonActive = comparisonMode != nullptr
                                  && comparisonMode->getSelectedId()
                                         != static_cast<int> (
                                             spectrumviewer::SpectrumComparisonMode::absolute)
                                  && compatibleReference;
    const auto delta = comparisonMode != nullptr
                       && comparisonMode->getSelectedId()
                              == static_cast<int> (
                                  spectrumviewer::SpectrumComparisonMode::deltaDb)
                       && compatibleReference;
    const auto fixed = amplitudeRangeMode->getSelectedId() == 2 && ! delta;
    amplitudeRangeMode->setEnabled (! delta);
    minimumDb->setEnabled (fixed);
    maximumDb->setEnabled (fixed);
    minimumDb->setVisible (fixed);
    maximumDb->setVisible (fixed);
    minimumDbLabel->setVisible (fixed);
    maximumDbLabel->setVisible (fixed);
    automaticRangeLabel->setVisible (! fixed);
    baselineDisplay->setEnabled (! comparisonActive);
    baselineDisplay->setTooltip (
        comparisonActive
            ? "Aperiodic display is unavailable while comparing with a reference"
            : "Show or subtract a robust broad spectral background; the PSD estimate is unchanged");
}

void SpectrumViewerEditor::applyAmplitudeRangeToCanvas()
{
    auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get());
    if (spectrumCanvas == nullptr)
        return;
    auto* plot = spectrumCanvas->getPlotPtr();
    plot->setFixedAmplitudeRange (static_cast<float> (minimumDb->getValue()),
                                  static_cast<float> (maximumDb->getValue()));
    plot->setAmplitudeRangeMode (
        static_cast<spectrumviewer::AmplitudeRangeMode> (
            amplitudeRangeMode->getSelectedId()));
}

void SpectrumViewerEditor::timerCallback()
{
    auto processor = static_cast<SpectrumViewer*> (getProcessor());
    const auto capture = processor->getCaptureState();
    captureStatusLabel->setTooltip ({});
    captureDuration->setEnabled (capture == SpectrumCaptureState::live
                                 || capture == SpectrumCaptureState::failed);
    analysisProfile->setEnabled (capture == SpectrumCaptureState::live
                                 || capture == SpectrumCaptureState::failed);
    captureAction->setEnabled (
        processor->getAnalysisReadiness() != SpectrumAnalysisReadiness::stopped
        && capture != SpectrumCaptureState::restoringLive);
    switch (capture)
    {
        case SpectrumCaptureState::preparing:
            captureAction->setButtonText ("Cancel");
            captureAction->setTooltip ("Cancel this capture and return to the live spectrum");
            captureStatusLabel->setText ("Preparing Fine...", dontSendNotification);
            captureStatusLabel->setTooltip (
                "Preparing 2 s, NW=3, K=4 non-overlapping Fine analysis");
            break;
        case SpectrumCaptureState::capturing:
            captureAction->setButtonText ("Cancel");
            captureAction->setTooltip ("Cancel this capture and return to the live spectrum");
            captureStatusLabel->setText (
                "Fine " + String (processor->getCaptureIncludedWindowCount()) + "/"
                    + String (processor->getCaptureTargetWindowCount()) + " ("
                    + String (processor->getCaptureAnalyzedSeconds(), 0) + " s)",
                dontSendNotification);
            captureStatusLabel->setTooltip (
                "2 s, NW=3, K=4 non-overlapping Fine spectra");
            break;
        case SpectrumCaptureState::frozen:
        {
            captureAction->setButtonText ("Go Live");
            captureAction->setTooltip ("Discard the frozen result and resume the live spectrum");
            const auto warning = processor->getCaptureFailedWindowCount()
                                     + processor->getCaptureShedWindowCount()
                                     + processor->getCaptureDiscontinuityCount()
                                 > 0;
            captureStatusLabel->setText (
                "Frozen " + String (processor->getCaptureAnalyzedSeconds(), 0)
                    + "/" + String (processor->getCaptureWallSpanSeconds(), 0)
                    + " s" + (warning ? " !" : ""),
                dontSendNotification);
            captureStatusLabel->setTooltip (
                "2 s, NW=3, K=4; analyzed / wall-span seconds; failed "
                + String (processor->getCaptureFailedWindowCount()) + ", shed "
                + String (processor->getCaptureShedWindowCount())
                + ", discontinuities "
                + String (processor->getCaptureDiscontinuityCount()));
            break;
        }
        case SpectrumCaptureState::restoringLive:
            captureAction->setButtonText ("Restoring...");
            captureAction->setTooltip ("Preparing the live analysis");
            captureStatusLabel->setText ("Frozen", dontSendNotification);
            break;
        case SpectrumCaptureState::failed:
            captureAction->setButtonText ("Retry");
            captureAction->setTooltip ("Retry the spectrum capture");
            captureStatusLabel->setText ("Capture failed", dontSendNotification);
            break;
        case SpectrumCaptureState::live:
        default:
            captureAction->setButtonText ("Capture");
            captureAction->setTooltip ("Average Fine spectra, then freeze the result");
            captureStatusLabel->setText ("Live", dontSendNotification);
            break;
    }

    String text;
    readinessLabel->setTooltip ({});
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
        case SpectrumAnalysisReadiness::stopped:
        default:
            text = "Stopped";
            break;
    }
    readinessLabel->setText (text, dontSendNotification);

    setReferenceAction->setEnabled (capture == SpectrumCaptureState::frozen);
    clearReferenceAction->setEnabled (processor->hasSpectrumReference());
    comparisonMode->setEnabled (processor->hasSpectrumReference());
    if (! processor->hasSpectrumReference())
        referenceStatusLabel->setText ("No reference", dontSendNotification);
    else
    {
        const auto compatibility = processor->getReferenceCompatibility();
        referenceStatusLabel->setText (
            (capture == SpectrumCaptureState::frozen
                 ? "Ref set; Go Live"
                 : "Ref ")
                + (capture == SpectrumCaptureState::frozen
                       ? String()
                       : Time (processor->getReferenceCapturedAtMilliseconds())
                             .formatted ("%H:%M:%S"))
                + (compatibility
                           == spectrumviewer::SpectrumReferenceCompatibility::incompatible
                       ? " (incompatible)"
                       : ""),
            dontSendNotification);
        referenceStatusLabel->setTooltip (
            compatibility == spectrumviewer::SpectrumReferenceCompatibility::incompatible
                ? "Select Fine analysis with the same channels, units, sample rate, detrending, NW, and K"
                : "Session-local immutable capture reference");
    }
    updateAmplitudeRangeControls();
    auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get());
    if (spectrumCanvas != nullptr && amplitudeRangeMode->getSelectedId() == 1)
    {
        auto* plot = spectrumCanvas->getPlotPtr();
        if (plot->hasAutomaticAmplitudeRange())
        {
            const auto range = plot->getAmplitudeRange();
            automaticRangeLabel->setText (String (range.minimum, 1) + " to "
                                              + String (range.maximum, 1) + " dB",
                                          dontSendNotification);
        }
        else
            automaticRangeLabel->setText ("Awaiting spectrum...",
                                          dontSendNotification);
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
    xml->setAttribute ("aperiodic_display", baselineDisplay->getSelectedId());
    xml->setAttribute ("show_peak_envelope", peakEnvelope->getToggleState());
    xml->setAttribute ("amplitude_range_mode", amplitudeRangeMode->getSelectedId());
    xml->setAttribute ("minimum_db", minimumDb->getValue());
    xml->setAttribute ("maximum_db", maximumDb->getValue());
    xml->setAttribute ("capture_duration", captureDuration->getSelectedId());
    xml->setAttribute ("comparison_mode", comparisonMode->getSelectedId());
    if (auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get()))
        xml->setAttribute ("options_drawer_open",
                           spectrumCanvas->isOptionsDrawerOpen());
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
    baselineDisplay->setSelectedId (
        jlimit (1, 3, xml->getIntAttribute ("aperiodic_display", 1)),
        sendNotification);
    peakEnvelope->setToggleState (
        xml->getBoolAttribute ("show_peak_envelope", true),
        sendNotification);
    minimumDb->setValue (xml->getDoubleAttribute ("minimum_db", -25.0),
                         dontSendNotification);
    maximumDb->setValue (xml->getDoubleAttribute ("maximum_db", 25.0),
                         dontSendNotification);
    if (maximumDb->getValue() - minimumDb->getValue() < 20.0)
    {
        minimumDb->setValue (-25.0, dontSendNotification);
        maximumDb->setValue (25.0, dontSendNotification);
    }
    amplitudeRangeMode->setSelectedId (
        xml->getIntAttribute ("amplitude_range_mode", 1), sendNotification);
    captureDuration->setSelectedId (
        jlimit (1, 3, xml->getIntAttribute ("capture_duration", 1)),
        dontSendNotification);
    comparisonMode->setSelectedId (
        jlimit (1, 3, xml->getIntAttribute ("comparison_mode", 1)),
        sendNotification);
    if (auto* spectrumCanvas = static_cast<SpectrumCanvas*> (canvas.get()))
        spectrumCanvas->setOptionsDrawerOpen (
            xml->getBoolAttribute ("options_drawer_open", false));
}
