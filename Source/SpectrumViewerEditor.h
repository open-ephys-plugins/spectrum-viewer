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
							 public ComboBox::Listener
{
	friend class SpectrumCanvas;
public:

	/** Constructor */
	SpectrumViewerEditor(GenericProcessor* parentNode);

	/** Destructor */
	~SpectrumViewerEditor() { }

	/** Enables animation */
	void startAcquisition() override;

	/** Disables animation*/
	void stopAcquisition() override;

	/** Called when a ComboBox changes*/
	void comboBoxChanged(ComboBox* comboBox);

	/** Creates the canvas */
	Visualizer* createNewCanvas();

	/** Notifies editor that the selected stream has changed.*/
	 void selectedStreamHasChanged() override;

	void saveVisualizerEditorParameters(XmlElement* xml) override;

	void loadVisualizerEditorParameters(XmlElement* xml) override;

private:

	std::unique_ptr<Label> displayLabel;
	std::unique_ptr<ComboBox> displayType;
	
	std::unique_ptr<Label> frequencyLabel;
	std::unique_ptr<ComboBox> frequencyRange;

	Array<Range<int>> freqRanges;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumViewerEditor);
};


#endif // SPECTRUM_VIEWER_EDITOR_H_INCLUDED