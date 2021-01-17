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


#include "CoherenceNodeEditor.h"

/************** editor *************/
CoherenceEditor::CoherenceEditor(CoherenceNode* p)
    : VisualizerEditor(p, 300, true), numChannels(0)
{
    tabText = "Spectrogram";
    processor = p;

    selectedSubprocessor = NULL_SUBPROCESSOR;

    channelASelection = new ComboBox("CH A ComboBox");
    channelASelection->setBounds(150, 55, 75, 22);
    channelASelection->addListener(this);
    channelASelection->addItem("-", 1);
    channelASelection->setSelectedId(1, dontSendNotification);
    addAndMakeVisible(channelASelection);

    channelALabel = new Label("CHANNEL A:", "CHANNEL A:");
    channelALabel->setFont(Font(Font::getDefaultSerifFontName(), 14, Font::plain));
    channelALabel->setBounds(150, 30, 85, 20);
    channelALabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(channelALabel);

    channelBSelection = new ComboBox("CH B ComboBox");
    channelBSelection->setBounds(150, 100, 75, 22);
    channelBSelection->addListener(this);
    channelBSelection->addItem("-", 1);
    channelBSelection->setSelectedId(1, dontSendNotification);
    addAndMakeVisible(channelBSelection);

    channelBLabel = new Label("CHANNEL B:", "CHANNEL B:");
    channelBLabel->setFont(Font(Font::getDefaultSerifFontName(), 14, Font::plain));
    channelBLabel->setBounds(150, 80, 85, 22);
    channelBLabel->setColour(Label::textColourId, Colours::darkgrey);
    addAndMakeVisible(channelBLabel);

    subprocessorSelectionLabel = new Label("Subprocessor", "Subprocessor:");
    subprocessorSelectionLabel->setBounds(10, 30, 130, 20);
    addAndMakeVisible(subprocessorSelectionLabel);

    subprocessorSelection = new ComboBox("Subprocessor selection");
    subprocessorSelection->setBounds(10, 55, 130, 22);
    subprocessorSelection->addListener(this);
    addAndMakeVisible(subprocessorSelection);

    subprocessorSampleRateLabel = new Label("Subprocessor sample rate label", "Sample Rate:");
    subprocessorSampleRateLabel->setFont(Font(Font::getDefaultSerifFontName(), 14, Font::plain));
    subprocessorSampleRateLabel->setBounds(subprocessorSelection->getX(), subprocessorSelection->getBottom() + 10, 200, 40);
    addAndMakeVisible(subprocessorSampleRateLabel);

    desiredWidth = 270;
    
}

CoherenceEditor::~CoherenceEditor() {}


void CoherenceEditor::comboBoxChanged(ComboBox* comboBoxThatHasChanged)
{
    if (comboBoxThatHasChanged == channelASelection)
    {
        getProcessor()->setParameter(0, channelASelection->getSelectedId() - 2 + startChannel);
    }
    else if (comboBoxThatHasChanged == channelBSelection)
    {
        getProcessor()->setParameter(1, channelBSelection->getSelectedId() - 2 + startChannel);
    } 
    else if (comboBoxThatHasChanged == subprocessorSelection)
    {
        std::cout << "Setting subprocessor to " << comboBoxThatHasChanged->getSelectedId() << std::endl;
        selectedSubprocessor = comboBoxThatHasChanged->getSelectedId();

        updateAvailableChannels();

        getProcessor()->setParameter(0, channelASelection->getSelectedId() - 2 + startChannel);
        getProcessor()->setParameter(1, channelBSelection->getSelectedId() - 2 + startChannel);

        processor->updateSubprocessor();

    }
}

void CoherenceEditor::updateAvailableChannels()
{
    int chAId = 1;
    int chBId = 1;

    if (channelASelection->getNumItems() > 0)
    {
        chAId = channelASelection->getSelectedId();
        chBId = channelBSelection->getSelectedId();
    }

    channelASelection->clear();
    channelASelection->addItem("-", 1);

    channelBSelection->clear();
    channelBSelection->addItem("-", 1);

    bool isFirstChanInSubprocessor = true;
    int totalSubprocessorChannels = -1;

    for (int ch = 0; ch < processor->getNumInputs(); ch++)
    {
        uint32 channelSubprocessor = processor->getDataSubprocId(ch);

        if (selectedSubprocessor == channelSubprocessor)
        {
            if (isFirstChanInSubprocessor)
            {
                String sampleRateLabelText = "Sample Rate: ";
                sampleRateLabelText += String(subprocessorSampleRate[selectedSubprocessor]);
                subprocessorSampleRateLabel->setText(sampleRateLabelText, dontSendNotification);

                startChannel = ch;

                isFirstChanInSubprocessor = false;
            }
 
            totalSubprocessorChannels++;

            channelASelection->addItem(String(ch + 1 - startChannel), ch - startChannel + 2);
            channelBSelection->addItem(String(ch + 1 - startChannel), ch - startChannel + 2);
        }
    }

    if (chAId == 1 && totalSubprocessorChannels > -1)
        channelASelection->setSelectedId(2, sendNotification); // update channel A
    else
        channelASelection->setSelectedId(chAId, sendNotification); // update channel A

    if (chBId == 1 && totalSubprocessorChannels > 0)
        channelBSelection->setSelectedId(3, sendNotification); // update channel B
    else
        channelBSelection->setSelectedId(chBId, sendNotification); // update channel B
}


void CoherenceEditor::updateSettings()
{

    std::cout << "Coherence Node updating settings" << std::endl;

    uint32 currentSubprocessor = NULL_SUBPROCESSOR;

    subprocessorSampleRate.clear();
    subprocessorSelection->clear();

    // Step 1: Populate subprocessor list
    for (int ch = 0; ch < processor->getNumInputs(); ch++)
    {
        uint32 channelSubprocessor = processor->getDataSubprocId(ch);

        if (currentSubprocessor != channelSubprocessor)
        {
            currentSubprocessor = channelSubprocessor;

            subprocessorSampleRate.insert({ channelSubprocessor, processor->getDataChannel(ch)->getSampleRate() });

            const DataChannel* channelInfo = processor->getDataChannel(ch);
            uint16 sourceNodeId = channelInfo->getSourceNodeID();
            uint16 subProcessorIdx = channelInfo->getSubProcessorIdx();
            uint32 subProcFullId = GenericProcessor::getProcessorFullId(sourceNodeId, subProcessorIdx);

            String sourceName = channelInfo->getSourceName();
            subprocessorSelection->addItem(sourceName + " " + String(sourceNodeId) + "/" + String(subProcessorIdx), currentSubprocessor);
        }
    }

    bool shouldUpdateSubprocessor = true;

    if (selectedSubprocessor != NULL_SUBPROCESSOR)
    {
        if (subprocessorSelection->indexOfItemId(selectedSubprocessor) > -1)
        {
            // already selected
            subprocessorSelection->setSelectedId(selectedSubprocessor, dontSendNotification);
            shouldUpdateSubprocessor = false;
        }
        else
        {
            subprocessorSelection->setSelectedItemIndex(0, dontSendNotification);
            selectedSubprocessor = subprocessorSelection->getSelectedId();
        } 
    }
    else
    {
        subprocessorSelection->setSelectedItemIndex(0, dontSendNotification);
        selectedSubprocessor = subprocessorSelection->getSelectedId();
    }

    updateAvailableChannels();

    if (shouldUpdateSubprocessor)
        processor->updateSubprocessor();
}

void CoherenceEditor::startAcquisition()
{
    subprocessorSelection->setEnabled(false);

    if (canvas != nullptr)
        canvas->beginAnimation();
}

void CoherenceEditor::stopAcquisition()
{
    subprocessorSelection->setEnabled(true);

    if (canvas != nullptr)
        canvas->endAnimation();
}

Visualizer* CoherenceEditor::createNewCanvas()
{
    canvas = new CoherenceVisualizer(processor);
    return canvas;
}



void CoherenceEditor::saveCustomParameters(XmlElement* xml)
{
    VisualizerEditor::saveCustomParameters(xml);

    std::cout << "Saving Spectrum Viewer editor." << std::endl;

    xml->setAttribute("Type", "CrossingDetectorEditor");
    XmlElement* paramValues = xml->createNewChildElement("VALUES");

    paramValues->setAttribute("subprocessor", subprocessorSelection->getSelectedId());
    paramValues->setAttribute("channelA", channelASelection->getSelectedId());
    paramValues->setAttribute("channelB", channelBSelection->getSelectedId());

}

void CoherenceEditor::loadCustomParameters(XmlElement* xml)
{

    VisualizerEditor::loadCustomParameters(xml);

    std::cout << "Loading Spectrum Viewer editor." << std::endl;

    forEachXmlChildElementWithTagName(*xml, xmlNode, "VALUES")
    {

        int subprocId = xmlNode->getIntAttribute("subprocessor");
        int chAId = xmlNode->getIntAttribute("channelA");
        int chBId = xmlNode->getIntAttribute("channelB");

        if (subprocessorSelection->indexOfItemId(subprocId) > -1)
            subprocessorSelection->setSelectedId(subprocId, sendNotification);

        if (channelASelection->indexOfItemId(chAId) > -1)
            channelASelection->setSelectedId(chAId, sendNotification);

        if (channelBSelection->indexOfItemId(chBId) > -1)
            channelBSelection->setSelectedId(chBId, sendNotification);

    }
}