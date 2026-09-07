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

#include "SpectrumCanvas.h"
#include <math.h>

#include <limits>

SpectrumCanvas::SpectrumCanvas (SpectrumViewer* n)
    : Visualizer ((GenericProcessor*) n), processor (n), displayType (POWER_SPECTRUM)
{
    refreshRate = 60;

    canvasPlot = std::make_unique<CanvasPlot> (processor);

    viewport = std::make_unique<Viewport>();
    viewport->setViewedComponent (canvasPlot.get(), true);
    viewport->setScrollBarsShown (true, true);
    viewport->setScrollBarThickness (12);
    addAndMakeVisible (viewport.get());
}

void SpectrumCanvas::resized()
{
    int plotWidth, plotHeight;

    viewport->setBounds (0, 0, getWidth(), getHeight());

    if (displayType == POWER_SPECTRUM)
    {
        if (viewport->getMaximumVisibleWidth() < 840 + canvasPlot->legendWidth)
            plotWidth = 800;
        else
            plotWidth = viewport->getMaximumVisibleWidth() - canvasPlot->legendWidth - 40;

        if (viewport->getMaximumVisibleHeight() < 650)
            plotHeight = 600;
        else
            plotHeight = viewport->getMaximumVisibleHeight() - 50;

        canvasPlot->setBounds (0, 0, plotWidth + canvasPlot->legendWidth + 40, plotHeight + 50);
    }
    else
    {
        canvasPlot->setBounds (0, 0, viewport->getMaximumVisibleWidth(), viewport->getMaximumVisibleHeight());
    }
}

void SpectrumCanvas::refreshState() {}

void SpectrumCanvas::paint (Graphics& g)
{
    // g.fillAll(Colour(28,28,28));
}

void SpectrumCanvas::updateSettings()
{
    canvasPlot->updateActiveChans();
}

void SpectrumCanvas::beginAnimation()
{
    canvasPlot->clear();
    startCallbacks();
}

void SpectrumCanvas::endAnimation()
{
    stopCallbacks();
}

void SpectrumCanvas::refresh()
{
    const auto readiness = processor->getAnalysisReadiness();
    const auto unavailable = readiness == SpectrumAnalysisReadiness::preparing
                             || readiness == SpectrumAnalysisReadiness::warmingUp
                             || (readiness == SpectrumAnalysisReadiness::configurationFailed
                                 && ! processor->hasActiveAnalysis());
    if (unavailable)
    {
        if (! unavailableStateCleared)
        {
            canvasPlot->clear();
            unavailableStateCleared = true;
        }
        return;
    }
    unavailableStateCleared = false;

    bool needsRedraw = false;

    processor->consumeLatestSpectrumFrame ([&] (const spectrumviewer::SpectrumFrameFifo::FrameView& frame)
                                           {
        canvasPlot->setBinWidth (static_cast<float> (frame.descriptor.binWidthHz));
        for (std::size_t channel = 0; channel < frame.numChannels; ++channel)
        {
            std::vector<float> power (frame.getChannelData (channel),
                                      frame.getChannelData (channel) + frame.numBins);
            if (displayType == POWER_SPECTRUM)
            {
                needsRedraw = true;
                canvasPlot->updatePowerSpectrum (std::move (power), static_cast<int> (channel));
            }
            else if (channel == 0)
                canvasPlot->drawSpectrogram (std::move (power));
        } });

    if (needsRedraw)
        canvasPlot->plotPowerSpectrum();
}

void SpectrumCanvas::setDisplayType (DisplayType type)
{
    if (CoreServices::getAcquisitionStatus())
    {
        stopCallbacks();
        displayType = type;
        canvasPlot->setDisplayType (type);
        startCallbacks();
    }
    else
    {
        displayType = type;
        canvasPlot->setDisplayType (type);
    }

    resized();
}

/** CANVAS PLOT - Stores the plot along with it's legend*/

CanvasPlot::CanvasPlot (SpectrumViewer* p)
    : processor (p), displayType (POWER_SPECTRUM), freqStep (4), nFreqs (250), freqEnd (1000)
{
    plt = std::make_unique<InteractivePlot>();
    plt->title ("POWER SPECTRUM");
    XYRange range { 0, 1000, 0, 5 };
    plt->setRange (range);
    plt->xlabel ("Frequency (Hz)");
    plt->ylabel ("PSD (dB)");
    plt->setBackgroundColour (Colour (45, 45, 45));
    plt->setGridColour (Colour (100, 100, 100));
    plt->setInteractive (InteractivePlotMode::OFF);
    addAndMakeVisible (plt.get());

    clearButton = std::make_unique<UtilityButton> ("Clear");
    clearButton->addListener (this);
    addAndMakeVisible (clearButton.get());

    activeChannels = processor->getActiveChans();

    spectrogramImg = std::make_unique<Image> (Image::RGB, 1000, 1000, true, SoftwareImageType());
    setOpaque (true);

    currPower.resize (MAX_CHANS);

    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].clear();
    }

    for (int i = 0; i < nFreqs; i++)
    {
        xvalues.push_back (i * freqStep);
    }
}

void CanvasPlot::resized()
{
    plt->setBounds (20, 30, getWidth() - legendWidth - 40, getHeight() - 50);
    clearButton->setBounds (plt->getRight() - 80, plt->getBottom() - 90, 60, 20);
}

void CanvasPlot::lookAndFeelChanged()
{
    plt->setBackgroundColour (findColour (ThemeColours::componentBackground));
    plt->setGridColour (findColour (ThemeColours::controlPanelText).withAlpha (0.5f));
    plt->setAxisColour (findColour (ThemeColours::controlPanelText));

    chanColors[0] = findColour (ThemeColours::defaultText);
    plt->plot (xvalues, currPower[0], chanColors[0], 1.0f);
}

void CanvasPlot::updateActiveChans()
{
    activeChannels = processor->getActiveChans();
    clear();
    repaint();
}

void CanvasPlot::setFrequencyRange (int freqStart_, int freqEnd_, float freqStep_)
{
    freqStep = freqStep_;
    freqStart = freqStart_;
    freqEnd = freqEnd_;
    nFreqs = (int) (freqEnd_ - freqStart_) / freqStep_;

    xvalues.clear();
    for (int i = 0; i < nFreqs; i++)
    {
        xvalues.push_back (i * freqStep);
    }

    XYRange range { (float) freqStart_, (float) freqEnd_, 0, 5 };
    plt->setRange (range);

    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
    }
}

void CanvasPlot::setBinWidth (float newBinWidth)
{
    if (std::isfinite (newBinWidth) && newBinWidth > 0.0f
        && std::abs (newBinWidth - freqStep) > std::numeric_limits<float>::epsilon())
        setFrequencyRange (freqStart, freqEnd, newBinWidth);
}

void CanvasPlot::setDisplayType (DisplayType type)
{
    displayType = type;

    if (displayType == SPECTROGRAM)
    {
        plt->setVisible (false);
        clearButton->setVisible (false);
    }
    else
    {
        plt->setVisible (true);
        clearButton->setVisible (true);
    }

    clear();
    repaint();
}

void CanvasPlot::plotPowerSpectrum()
{
    plt->clear();

    auto minimum = std::numeric_limits<float>::infinity();
    auto maximum = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < activeChannels.size(); i++)
    {
        for (const auto value : currPower[static_cast<std::size_t> (i)])
        {
            if (std::isfinite (value))
            {
                minimum = std::min (minimum, value);
                maximum = std::max (maximum, value);
            }
        }
        plt->plot (xvalues, currPower[i], chanColors[i], 1.0f);
    }

    if (std::isfinite (minimum) && std::isfinite (maximum))
    {
        const auto padding = std::max (1.0f, 0.05f * (maximum - minimum));
        XYRange range;
        plt->getRange (range);
        range.ymin = minimum - padding;
        range.ymax = maximum + padding;
        plt->setRange (range);
    }
}

void CanvasPlot::updatePowerSpectrum (std::vector<float> powerData, int channelIndex)
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (currPower.size()))
        return;
    powerData.resize (std::min (powerData.size(), currPower[static_cast<std::size_t> (channelIndex)].size()));

    auto& destination = currPower[static_cast<std::size_t> (channelIndex)];
    for (std::size_t n = 0; n < powerData.size(); ++n)
    {
        const auto power = powerData[n];
        if (std::isfinite (power) && power > 0.0f)
        {
            const auto decibels = 10.0f * std::log10 (power);
            destination[n] = decibels;
        }
        else
            destination[n] = std::numeric_limits<float>::quiet_NaN();
    }
}

void CanvasPlot::drawSpectrogram (std::vector<float> chanData)
{
    chanData.resize (std::min (chanData.size(), static_cast<std::size_t> (std::max (0, nFreqs))));
    if (chanData.empty())
        return;

    auto imageWidth = spectrogramImg->getWidth() - 1;
    auto imageHeight = spectrogramImg->getHeight();

    // first, shuffle our image rightwards by 1 pixel..
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth, imageHeight);

    // find the range of values produced, so we can scale our rendering to
    // show up the detail clearly
    auto powerRange = juce::FloatVectorOperations::findMinAndMax (chanData.data(), chanData.size());

    if (std::isfinite (powerRange.getStart()) == false || std::isfinite (powerRange.getEnd()) == false)
        return;

    for (auto y = 0; y < imageHeight - 1; ++y)
    {
        auto skewedProportionY = 1.0f - (float) y / (float) imageHeight;
        auto dataIndex = (size_t) jlimit (0, (int) (chanData.size() - 1), (int) (skewedProportionY * (chanData.size() - 1)));

        float logPower = chanData[dataIndex] > 0.0f ? std::log10 (1.0f + chanData[dataIndex]) : 0.0f;
        float logMax = powerRange.getEnd() > 0.0f ? std::log10 (1.0f + powerRange.getEnd()) : 0.0f;
        float logMin = powerRange.getStart() > 0.0f ? std::log10 (1.0f + powerRange.getStart()) : 0.0f;

        auto level = juce::jmap (logPower, logMin, juce::jmax (logMax, 1e-5f), 0.0f, 1.0f);

        spectrogramImg->setPixelAt (0, y, juce::Colour::fromHSV (level, 1.0f, level, 1.0f));
    }

    repaint();
}

void CanvasPlot::paint (Graphics& g)
{
    g.fillAll (findColour (ThemeColours::componentParentBackground));

    if (displayType == POWER_SPECTRUM)
    {
        if (activeChannels.size() == 0)
            return;

        int left = getWidth() - legendWidth - 10;
        int top = 60;

        g.setFont (FontOptions ("Inter", "Semi Bold", 16.0f));

        for (int i = 0; i < activeChannels.size(); i++)
        {
            top = (i + 1) * rowHeight + 10;

            g.setColour (chanColors.at (i));
            g.fillRect (left, top + 10, 30, 30);

            g.setColour (findColour (ThemeColours::controlPanelText));
            String chan = processor->getChanName (activeChannels[i]);
            g.drawFittedText (chan, left + 45, top + 10, (legendWidth - 20) / 2, 30, Justification::centredLeft, 1);

            g.setColour (findColour (ThemeColours::defaultFill));
            g.drawRect (left, top + 10, 30, 30, 2);
        }
    }
    else
    {
        g.setColour (findColour (ThemeColours::controlPanelText));

        int w = 50;
        int h = getHeight();

        int padding = 10;

        g.drawLine (w - 3, padding, w - 3, h - padding, 2.0);

        int ticklabelWidth = 60;
        int tickLabelHeight = 20;

        g.setFont (FontOptions ("Inter", "Regular", 12.0f));

        for (int k = 0; k <= 10; k++)
        {
            float ytickloc = padding + ((k) * (h - padding * 2) / 10);

            ytickloc = h - ytickloc;

            g.drawLine (w - 13, ytickloc, w - 3, ytickloc, 2.0);

            String yTick;

            if (k != 0)
                yTick = String ((freqEnd * k) / 10);
            else
                yTick = String (0);

            g.drawText (yTick,
                        0,
                        ytickloc - tickLabelHeight / 2,
                        w - 15,
                        tickLabelHeight,
                        Justification::right,
                        false);
        }

        auto imgBounds = getLocalBounds();
        imgBounds.setLeft (60);
        imgBounds.setRight (getWidth() - 10);
        imgBounds.setBottom (getHeight() - 10);
        imgBounds.setTop (10);
        g.drawImage (*spectrogramImg, imgBounds.toFloat());
    }
}

void CanvasPlot::buttonClicked (Button* button)
{
    if (button == clearButton.get())
    {
        clear();
    }
}

void CanvasPlot::clear()
{
    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
    }

    spectrogramImg->clear (spectrogramImg->getBounds());
    plt->clear();
}
