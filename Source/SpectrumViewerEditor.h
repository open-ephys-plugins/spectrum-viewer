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

#include "SpectrumDisplaySettings.h"

/**
    Signal-chain editor for the Spectrum Viewer.

    Holds only what belongs in the signal chain: stream and channel selection,
    which change the processor's input route, and a readiness label. Every
    display control lives on the canvas.

    The editor does own the display *settings*, because the canvas is created
    lazily and may never exist; see SpectrumDisplaySettings.
*/
class SpectrumViewerEditor : public VisualizerEditor,
                             private Timer
{
public:
    /** Constructor */
    SpectrumViewerEditor (GenericProcessor* parentNode);

    /** Destructor */
    ~SpectrumViewerEditor() override;

    /** Enables animation */
    void startAcquisition() override;

    /** Disables animation*/
    void stopAcquisition() override;

    /** Creates the canvas */
    Visualizer* createNewCanvas() override;

    /** Notifies editor that the selected stream has changed.*/
    void selectedStreamHasChanged() override;

    void saveVisualizerEditorParameters (XmlElement* xml) override;

    void loadVisualizerEditorParameters (XmlElement* xml) override;

    /** Display state, edited by the canvas and persisted here. */
    SpectrumDisplaySettings& getDisplaySettings() noexcept { return displaySettings; }

private:
    void timerCallback() override;

    SpectrumDisplaySettings displaySettings;
    std::unique_ptr<Label> readinessLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumViewerEditor);
};

#endif // SPECTRUM_VIEWER_EDITOR_H_INCLUDED
