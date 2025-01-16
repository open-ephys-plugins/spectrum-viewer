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

#ifndef SPECTRUMCANVAS_H_INCLUDED
#define SPECTRUMCANVAS_H_INCLUDED

#include <VisualizerWindowHeaders.h>

#include "AtomicSynchronizer.h"
#include "SpectrumViewer.h"

#include <DspLib.h>

class SpectrumCanvas;

// Component for housing power spectrum & spectrograph plots
class CanvasPlot : public Component
				 , public Button::Listener
{
public:

	/** Constructor */
	CanvasPlot(SpectrumViewer* p);

	/** Destructor */
	~CanvasPlot() { }

	/** Draws the canvas */
	void paint(Graphics& g) override;

	/** Updates component boundaries */
	void resized() override;

	/** Called when the look and feel changes */
	void lookAndFeelChanged() override;

	void updateActiveChans();

	void setFrequencyRange(int freqStart, int freqEnd, float freqStep);

	void updatePowerSpectrum(std::vector<float> powerData, int channelIndex);

	void plotPowerSpectrum();

	void drawSpectrogram(std::vector<float> powerData);

	/** Sets the display type for the canvas (Power Spectrum or Spectrogram)*/
	void setDisplayType(DisplayType type);

	/** Called when a button is clicked */
	void buttonClicked(Button* button) override;

	/** Clears the plot */
	void clear();

	int legendWidth = 150;

	DisplayType displayType;

private:

	std::vector<Colour> chanColors = { Colour(200, 200, 200)
										, Colour(230, 159, 0)
										, Colour(86, 180, 233)
										, Colour(0, 158, 115)
										, Colour(240, 228, 66)
										, Colour(0, 114, 178)
										, Colour(242, 66, 53)
										, Colour(204, 121, 167) };
	
	std::unique_ptr<UtilityButton> clearButton;

	SpectrumViewer* processor;
	
	int rowHeight = 50;

	float maxPower = 0.0f;

	std::vector<std::vector<float>> currPower; // channels x freqs

	std::vector<float> xvalues;

	InteractivePlot plt;

	float freqStep;
	int nFreqs;
	int freqEnd;

	Array<int> activeChannels;

	/** Image to draw*/
	std::unique_ptr<Image> spectrogramImg;

	OwnedArray<OwnedArray<Dsp::Filter>> lowPassFilters;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CanvasPlot);
};

/** 

	Draws the real-time power spectrum

*/
class SpectrumCanvas : public Visualizer
{
public:

	/** Constructor */
	SpectrumCanvas(SpectrumViewer* n);

	/** Destructor */
	~SpectrumCanvas() { }

	/** Called when tab becomes visible again */
	void refreshState();

	/** Updates settings */
	void updateSettings() override;

	/** Called when data acquisition begins */
    void beginAnimation() override;

    /** Called when data acquisition ends */
    void endAnimation() override;

	/** Called instead of repaint to avoid re-painting sub-components*/
	void refresh() override;

	/** Draws the canvas */
	void paint(Graphics& g) override;

	/** Updates component boundaries */
	void resized() override;

	/** Sets the display type for the canvas (Power Spectrum or Spectrogram)*/
	void setDisplayType(DisplayType type);

	CanvasPlot* getPlotPtr() { return canvasPlot.get(); };

private:

	SpectrumViewer* processor;

	std::unique_ptr<Viewport>  viewport;
	std::unique_ptr<CanvasPlot> canvasPlot;
	juce::Rectangle<int> canvasBounds;

	DisplayType displayType;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectrumCanvas);
};

#endif // SPECTRUMCANVAS_H_INCLUDED