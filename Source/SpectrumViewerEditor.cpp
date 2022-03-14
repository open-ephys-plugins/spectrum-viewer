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


SpectrumViewerEditor::SpectrumViewerEditor(GenericProcessor* p)
    : VisualizerEditor(p, 200)
{
    
    tabText = "Power Spectrum";

    addSelectedChannelsParameterEditor("Channels", 20, 105);
    
}

Visualizer* SpectrumViewerEditor::createNewCanvas()
{
    return new SpectrumCanvas((SpectrumViewer*)getProcessor());;
}
