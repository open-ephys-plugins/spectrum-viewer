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

#ifndef SPECTRUM_VIEWER_EDITOR_H_INCLUDED
#define SPECTRUM_VIEWER_EDITOR_H_INCLUDED

#include <VisualizerEditorHeaders.h>

class SpectrumViewerEditor : public VisualizerEditor,
                             public ComboBox::Listener,
                             public Slider::Listener,
                             public Button::Listener,
                             private Timer
{
    friend class SpectrumCanvas;

public:
    /** Constructor */
    SpectrumViewerEditor (GenericProcessor* parentNode);

    /** Destructor */
    ~SpectrumViewerEditor() override;

    /** Enables animation */
    void startAcquisition() override;

    /** Disables animation*/
    void stopAcquisition() override;

    /** Called when a ComboBox changes*/
    void comboBoxChanged (ComboBox* comboBox);

    void sliderValueChanged (Slider* slider) override;

    void buttonClicked (Button* button) override;

    /** Creates the canvas */
    Visualizer* createNewCanvas();

    /** Notifies editor that the selected stream has changed.*/
    void selectedStreamHasChanged() override;

    void saveVisualizerEditorParameters (XmlElement* xml) override;

    void loadVisualizerEditorParameters (XmlElement* xml) override;

private:
    void timerCallback() override;
    void updateAmplitudeRangeControls();
    void applyAmplitudeRangeToCanvas();

    std::unique_ptr<Label> displayLabel;
    std::unique_ptr<ComboBox> displayType;

    std::unique_ptr<Label> frequencyLabel;
    std::unique_ptr<ComboBox> frequencyRange;

    std::unique_ptr<Label> profileLabel;
    std::unique_ptr<ComboBox> analysisProfile;
    std::unique_ptr<Label> scaleLabel;
    std::unique_ptr<ComboBox> frequencyScale;
    std::unique_ptr<Label> amplitudeLabel;
    std::unique_ptr<ComboBox> amplitudeDisplay;
    std::unique_ptr<Label> baselineLabel;
    std::unique_ptr<ComboBox> baselineDisplay;
    std::unique_ptr<ToggleButton> peakEnvelope;
    std::unique_ptr<Label> amplitudeRangeLabel;
    std::unique_ptr<ComboBox> amplitudeRangeMode;
    std::unique_ptr<Label> minimumDbLabel;
    std::unique_ptr<Slider> minimumDb;
    std::unique_ptr<Label> maximumDbLabel;
    std::unique_ptr<Slider> maximumDb;
    std::unique_ptr<Label> automaticRangeLabel;
    std::unique_ptr<Label> captureDurationLabel;
    std::unique_ptr<ComboBox> captureDuration;
    std::unique_ptr<UtilityButton> captureAction;
    std::unique_ptr<Label> captureStatusLabel;
    std::unique_ptr<ComboBox> comparisonMode;
    std::unique_ptr<Label> comparisonModeLabel;
    std::unique_ptr<UtilityButton> setReferenceAction;
    std::unique_ptr<UtilityButton> clearReferenceAction;
    std::unique_ptr<Label> referenceStatusLabel;
    std::unique_ptr<Label> readinessLabel;

    Array<Range<int>> freqRanges;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumViewerEditor);
};

#endif // SPECTRUM_VIEWER_EDITOR_H_INCLUDED
