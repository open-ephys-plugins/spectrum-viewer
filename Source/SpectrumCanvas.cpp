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
#include <algorithm>
#include <cmath>

SpectrumCanvas::SpectrumCanvas (SpectrumViewer* n)
    : Visualizer ((GenericProcessor*) n), processor (n), displayType (POWER_SPECTRUM)
{
    refreshRate = 60;

    canvasPlot = std::make_unique<CanvasPlot> (processor);

    viewport = std::make_unique<Viewport>();
    // canvasPlot remains owned by its unique_ptr; the viewport only displays it.
    viewport->setViewedComponent (canvasPlot.get(), false);
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

        const int minimumPlotHeight = canvasPlot->getMinimumHeight();
        if (viewport->getMaximumVisibleHeight() < minimumPlotHeight + 50)
            plotHeight = minimumPlotHeight;
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
    const auto frequencyRange = processor->getFrequencyRange();
    canvasPlot->setFrequencyRange (frequencyRange.getStart(), frequencyRange.getEnd(), processor->getFreqStep());
    resized();
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
    bool needsRedraw = false;

    // Pull at most the newest spectrum from each channel. Intermediate worker
    // updates may be skipped intentionally when rendering cannot keep up.
    const int channelCount = jmin (processor->getNumActiveChannels(), MAX_SPECTRUM_CHANNELS);
    for (int i = 0; i < channelCount; i++)
    {
        SpectrumViewer::PowerBuffer* buffer = &processor->powerBuffers[i];

        if (buffer->power.hasUpdate())
        {
            AtomicScopedReadPtr<std::vector<float>> powerReader (buffer->power);

            powerReader.pullUpdate();

            if (powerReader.isValid())
            {
                if (displayType == POWER_SPECTRUM)
                {
                    needsRedraw = true;
                    canvasPlot->updatePowerSpectrum (powerReader.operator*(), i);
                }
                else if (i == 0)
                    canvasPlot->drawSpectrogram (powerReader.operator*());
            }
        }
    }

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
    : processor (p), displayType (POWER_SPECTRUM), freqStep (4), nFreqs (250), freqStart (0), freqEnd (1000)
{
    plt = std::make_unique<InteractivePlot>();
    plt->title ("POWER SPECTRUM");
    XYRange range { 0, 1000, 0, 5 };
    plt->setRange (range);
    plt->xlabel ("Frequency (Hz)");
    plt->ylabel ("Power");
    plt->setBackgroundColour (Colour (45, 45, 45));
    plt->setGridColour (Colour (100, 100, 100));
    plt->setInteractive (InteractivePlotMode::OFF);
    addAndMakeVisible (plt.get());

    clearButton = std::make_unique<UtilityButton> ("Clear");
    clearButton->addListener (this);
    addAndMakeVisible (clearButton.get());

    activeChannels = processor->getActiveChans();
    ensureChannelColours();

    spectrogramImg = std::make_unique<Image> (Image::RGB, 1000, 1000, true, SoftwareImageType());
    setOpaque (true);

    currPower.resize (MAX_SPECTRUM_CHANNELS);
    temporalFilterState1.resize (MAX_SPECTRUM_CHANNELS);
    temporalFilterState2.resize (MAX_SPECTRUM_CHANNELS);
    powerScratch.resize (MAX_SPECTRUM_CHANNELS);
    renderPower.resize (MAX_SPECTRUM_CHANNELS);

    // One shared coefficient set replaces a heap-allocated filter object for
    // every bin while preserving the 1 Hz, second-order Butterworth response.
    constexpr float updateRateHz = 50.0f;
    constexpr float cutoffHz = 1.0f;
    const float k = std::tan (MathConstants<float>::pi * cutoffHz / updateRateHz);
    const float norm = 1.0f / (1.0f + std::sqrt (2.0f) * k + k * k);
    temporalB0 = k * k * norm;
    temporalB1 = 2.0f * temporalB0;
    temporalB2 = temporalB0;
    temporalA1 = 2.0f * (k * k - 1.0f) * norm;
    temporalA2 = (1.0f - std::sqrt (2.0f) * k + k * k) * norm;

    for (int ch = 0; ch < MAX_SPECTRUM_CHANNELS; ch++)
        currPower[ch].clear();

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
    repaint();
}

void CanvasPlot::updateActiveChans()
{
    activeChannels = processor->getActiveChans();
    ensureChannelColours();
    clear();
    repaint();
}

void CanvasPlot::setFrequencyRange (int freqStart_, int freqEnd_, float freqStep_)
{
    // Sanitize externally supplied values before they determine vector sizes.
    freqStart = jmax (0, freqStart_);
    freqEnd = jmax (freqStart + 1, freqEnd_);
    freqStep = std::isfinite (freqStep_) && freqStep_ > 0.0f ? freqStep_ : 1.0f;
    nFreqs = jmax (1, (int) ((freqEnd - freqStart) / freqStep));

    xvalues.resize ((size_t) nFreqs);
    for (int i = 0; i < nFreqs; i++)
        xvalues[(size_t) i] = freqStart + i * freqStep;

    XYRange range { (float) freqStart, (float) freqEnd, 0, 5 };
    plt->setRange (range);

    for (int ch = 0; ch < MAX_SPECTRUM_CHANNELS; ch++)
    {
        currPower[ch].assign ((size_t) nFreqs, 0.0f);
        temporalFilterState1[ch].assign ((size_t) nFreqs, 0.0f);
        temporalFilterState2[ch].assign ((size_t) nFreqs, 0.0f);
        powerScratch[ch].assign ((size_t) nFreqs, 0.0f);
        renderPower[ch].resize ((size_t) nFreqs);
    }

    renderXvalues.resize ((size_t) nFreqs);
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

    const int channelCount = jmin (activeChannels.size(), (int) currPower.size());

    float currentMaxPower = 0.0f;
    for (int channel = 0; channel < channelCount; ++channel)
    {
        const auto& channelPower = currPower[(size_t) channel];
        if (! channelPower.empty())
            currentMaxPower = jmax (currentMaxPower, FloatVectorOperations::findMinAndMax (channelPower.data(), channelPower.size()).getEnd());
    }

    // Expand immediately, but contract only when the held range is more than
    // twice what the current data needs. The extra headroom after contraction
    // provides hysteresis so small power changes do not make the axis jitter.
    constexpr float minimumMaxPower = 5.0f;
    constexpr float contractionThreshold = 2.0f;
    constexpr float contractionHeadroom = 1.25f;
    const float currentTarget = jmax (minimumMaxPower, currentMaxPower * 1.05f);

    maxPower = jmax (maxPower, currentTarget);
    if (maxPower > currentTarget * contractionThreshold)
        maxPower = currentTarget * contractionHeadroom;

    const float targetMaxPower = maxPower;

    XYRange plotRange;
    plt->getRange (plotRange);
    if (targetMaxPower != plotRange.ymax)
    {
        plotRange.ymax = targetMaxPower;
        plt->setRange (plotRange);
    }

    // Plot no more than roughly one point per horizontal pixel. Taking each
    // bucket's peak preserves narrow spectral features during decimation.
    const int pointCount = (int) xvalues.size();
    const int maxVisiblePoints = jmax (2, plt->getWidth());
    const int bucketSize = jmax (1, (pointCount + maxVisiblePoints - 1) / maxVisiblePoints);

    if (bucketSize == 1)
    {
        for (int channel = 0; channel < channelCount; ++channel)
            plt->plot (xvalues, currPower[(size_t) channel], chanColors[(size_t) channel], 1.0f);
        return;
    }

    const int renderedPointCount = (pointCount + bucketSize - 1) / bucketSize;
    renderXvalues.resize ((size_t) renderedPointCount);

    for (int point = 0; point < renderedPointCount; ++point)
    {
        const int firstBin = point * bucketSize;
        const int lastBin = jmin (pointCount, firstBin + bucketSize);
        renderXvalues[(size_t) point] = xvalues[(size_t) ((firstBin + lastBin - 1) / 2)];
    }

    for (int channel = 0; channel < channelCount; ++channel)
    {
        auto& reducedPower = renderPower[(size_t) channel];
        reducedPower.resize ((size_t) renderedPointCount);

        for (int point = 0; point < renderedPointCount; ++point)
        {
            const int firstBin = point * bucketSize;
            const int lastBin = jmin (pointCount, firstBin + bucketSize);
            reducedPower[(size_t) point] = *std::max_element (currPower[(size_t) channel].begin() + firstBin,
                                                              currPower[(size_t) channel].begin() + lastBin);
        }

        plt->plot (renderXvalues, reducedPower, chanColors[(size_t) channel], 1.0f);
    }
}

void CanvasPlot::updatePowerSpectrum (const std::vector<float>& powerData, int channelIndex)
{
    if (channelIndex < 0 || channelIndex >= (int) currPower.size())
        return;

    auto& smoothedPower = currPower[(size_t) channelIndex];
    auto& filterState1 = temporalFilterState1[(size_t) channelIndex];
    auto& filterState2 = temporalFilterState2[(size_t) channelIndex];
    auto& powerBuffer = powerScratch[(size_t) channelIndex];
    const auto valueCount = jmin (powerData.size(), smoothedPower.size());

    for (size_t n = 0; n < valueCount; ++n)
    {
        if (std::isfinite (powerData[n]))
        {
            // Transposed direct-form II uses two contiguous state values per
            // bin and is algebraically equivalent to the previous DSP filter.
            const float filteredPower = temporalB0 * powerData[n] + filterState1[n];
            filterState1[n] = temporalB1 * powerData[n] - temporalA1 * filteredPower + filterState2[n];
            filterState2[n] = temporalB2 * powerData[n] - temporalA2 * filteredPower;

            powerBuffer[n] = filteredPower >= 1.0f ? std::log (filteredPower) : smoothedPower[n];
        }
        else
            powerBuffer[n] = smoothedPower[n];
    }

    // A short moving average reduces bin-to-bin noise without hiding broad peaks.
    constexpr float frequencySmoothing = 1.0f / 9.0f;
    for (size_t n = 0; n < valueCount; ++n)
    {
        float value = 0.0f;
        for (int offset = -4; offset <= 4; ++offset)
        {
            const auto index = (size_t) jlimit (0, (int) valueCount - 1, (int) n + offset);
            value += powerBuffer[index] * frequencySmoothing;
        }

        smoothedPower[n] = value;
    }
}

void CanvasPlot::drawSpectrogram (const std::vector<float>& chanData)
{
    if (chanData.empty())
        return;

    auto imageWidth = spectrogramImg->getWidth() - 1;
    auto imageHeight = spectrogramImg->getHeight();

    // Shift history right and draw the newest spectrum into the left column.
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth, imageHeight);

    // Normalize each frame in log space so low-amplitude structure remains visible.
    auto powerRange = juce::FloatVectorOperations::findMinAndMax (chanData.data(), chanData.size());

    if (std::isfinite (powerRange.getStart()) == false || std::isfinite (powerRange.getEnd()) == false)
        return;

    const float logMin = powerRange.getStart() > 0.0f ? std::log10 (1.0f + powerRange.getStart()) : 0.0f;
    const float logMax = powerRange.getEnd() > 0.0f ? std::log10 (1.0f + powerRange.getEnd()) : 0.0f;
    const float logRange = jmax (1.0e-5f, logMax - logMin);

    for (auto y = 0; y < imageHeight; ++y)
    {
        const auto skewedProportionY = 1.0f - (float) y / (float) jmax (1, imageHeight - 1);
        auto dataIndex = (size_t) jlimit (0, (int) (chanData.size() - 1), (int) (skewedProportionY * (chanData.size() - 1)));

        const float logPower = chanData[dataIndex] > 0.0f ? std::log10 (1.0f + chanData[dataIndex]) : 0.0f;
        const float level = jlimit (0.0f, 1.0f, (logPower - logMin) / logRange);

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

        const int channelCount = jmin (activeChannels.size(), (int) chanColors.size());
        for (int i = 0; i < channelCount; ++i)
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

        int tickLabelHeight = 20;

        g.setFont (FontOptions ("Inter", "Regular", 12.0f));

        for (int k = 0; k <= 10; k++)
        {
            float ytickloc = padding + ((k) * (h - padding * 2) / 10);

            ytickloc = h - ytickloc;

            g.drawLine (w - 13, ytickloc, w - 3, ytickloc, 2.0);

            String yTick;

            yTick = String (freqStart + ((freqEnd - freqStart) * k) / 10);

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
    for (int ch = 0; ch < MAX_SPECTRUM_CHANNELS; ch++)
    {
        std::fill (currPower[ch].begin(), currPower[ch].end(), 0.0f);
        std::fill (temporalFilterState1[ch].begin(), temporalFilterState1[ch].end(), 0.0f);
        std::fill (temporalFilterState2[ch].begin(), temporalFilterState2[ch].end(), 0.0f);
        std::fill (powerScratch[ch].begin(), powerScratch[ch].end(), 0.0f);
    }

    maxPower = 0.0f;

    spectrogramImg->clear (spectrogramImg->getBounds());
    plt->clear();
}

int CanvasPlot::getMinimumHeight() const
{
    return jmax (600, activeChannels.size() * rowHeight + 60);
}

void CanvasPlot::ensureChannelColours()
{
    // Golden-ratio hue spacing keeps adjacent dynamically added channels distinct.
    while (chanColors.size() < (size_t) activeChannels.size())
    {
        const float hue = std::fmod (0.61803398875f * (float) chanColors.size(), 1.0f);
        chanColors.push_back (Colour::fromHSV (hue, 0.65f, 0.95f, 1.0f));
    }
}