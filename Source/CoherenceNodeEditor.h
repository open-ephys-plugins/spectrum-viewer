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

#ifndef COHERENCE_NODE_EDITOR_H_INCLUDED
#define COHERENCE_NODE_EDITOR_H_INCLUDED

#include "CoherenceNode.h"
#include "CoherenceVisualizer.h"

class CoherenceEditor
    : public VisualizerEditor
    , public ComboBox::Listener

{
    friend class CoherenceVisualizer;
public:
    CoherenceEditor(CoherenceNode* n);
    ~CoherenceEditor();

    void comboBoxChanged(ComboBox* comboBoxThatHasChanged) override;

    void startAcquisition() override;
    void stopAcquisition() override;

    Visualizer* createNewCanvas() override;

    void updateSettings() override;

private:
    CoherenceNode* processor;

    int numChannels;

    ScopedPointer<Label> ch1Label;
    ScopedPointer<ComboBox> ch1Selection;

    ScopedPointer<Label> ch2Label;
    ScopedPointer<ComboBox> ch2Selection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CoherenceEditor);
};


#endif // COHERENCE_NODE_EDITOR_H_INCLUDED