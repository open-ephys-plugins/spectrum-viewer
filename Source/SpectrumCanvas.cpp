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
#include "SpectrumViewerEditor.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace
{
class RotatedAxisLabel final : public Label
{
public:
    using Label::Label;

    void paint (Graphics& graphics) override
    {
        graphics.saveState();
        graphics.addTransform (AffineTransform::rotation (
            -MathConstants<float>::halfPi,
            static_cast<float> (getWidth()) * 0.5f,
            static_cast<float> (getHeight()) * 0.5f));
        graphics.setColour (findColour (Label::textColourId));
        graphics.setFont (getFont());
        graphics.drawFittedText (getText(),
                                 (getWidth() - getHeight()) / 2,
                                 (getHeight() - getWidth()) / 2,
                                 getHeight(),
                                 getWidth(),
                                 Justification::centred,
                                 1);
        graphics.restoreState();
    }
};

class ShowHideSpectrumOptionsButton final : public Button
{
public:
    ShowHideSpectrumOptionsButton()
        : Button ("Display options")
    {
        setClickingTogglesState (true);
        setTooltip ("Show or hide capture, comparison, and dB range controls");
    }

    void paintButton (Graphics& graphics, bool isMouseOver, bool) override
    {
        graphics.setColour (findColour (ThemeColours::defaultText)
                                .withAlpha (isMouseOver ? 1.0f : 0.85f));
        graphics.setFont (FontOptions ("Inter", "Regular", 12.0f));
        graphics.drawText ("Options", 0, 0, getWidth() - 22, getHeight(),
                           Justification::centredRight, false);

        const auto centreX = static_cast<float> (getWidth() - 11);
        const auto centreY = static_cast<float> (getHeight()) * 0.5f;
        Path arrow;
        if (getToggleState())
            arrow.addTriangle (centreX, centreY - 5.0f,
                               centreX - 6.0f, centreY + 4.0f,
                               centreX + 6.0f, centreY + 4.0f);
        else
            arrow.addTriangle (centreX - 4.0f, centreY - 6.0f,
                               centreX + 5.0f, centreY,
                               centreX - 4.0f, centreY + 6.0f);
        graphics.fillPath (arrow.createPathWithRoundedCorners (2.0f));
    }
};

} // namespace

FrequencyPlot::FrequencyPlot()
{
    yLabel = std::make_unique<RotatedAxisLabel> ("Y-Axis Label", "Y Label");
    yLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));
    yLabel->setJustificationType (Justification::centred);
    addAndMakeVisible (yLabel.get());
}

void FrequencyPlot::resized()
{
    InteractivePlot::resized();

    const auto width = getWidth();
    const auto height = getHeight();
    if (width <= 0 || height <= 0)
        return;

    constexpr int titleHeight = 20;
    constexpr int axesWidth = 78;
    constexpr int axesHeight = 50;
    constexpr int padding = 10;
    constexpr int yLabelWidth = 22;

    titleLabel->setBounds (axesWidth + padding + 2, 0,
                           width - axesWidth - padding * 2, titleHeight);
    xLabel->setBounds (axesWidth + padding + 2, height - 20,
                       width - axesWidth - padding * 2, 20);
    yLabel->setBounds (0, titleHeight, yLabelWidth,
                       height - axesHeight - titleHeight);

    yAxis->setBounds (yLabelWidth, titleHeight,
                      axesWidth - yLabelWidth - 4,
                      height - axesHeight - titleHeight);
    xAxis->setBounds (axesWidth + 1, height - axesHeight - 4,
                      width - axesWidth, axesHeight - 4);
    drawComponent->setBounds (axesWidth + padding + 2,
                              titleHeight + padding,
                              width - axesWidth - 2 - padding * 2,
                              height - axesHeight - titleHeight - padding * 2);
    autoRescaleButton->setBounds (axesWidth + padding + 2, titleHeight + padding,
                                  50, 20);
}

void FrequencyPlot::plot (std::vector<float> x,
                          std::vector<float> y,
                          Colour colour,
                          float width,
                          float opacity,
                          PlotType type)
{
    if (type != PlotType::LINE)
    {
        InteractivePlot::plot (std::move (x), std::move (y), colour, width, opacity, type);
        return;
    }

    lineSeries.push_back ({ std::move (x), std::move (y), colour, width, opacity });
    repaint (drawComponent->getBounds());
}

void FrequencyPlot::clear()
{
    lineSeries.clear();
    InteractivePlot::clear();
}

void FrequencyPlot::setFrequencyAxis (spectrumviewer::FrequencyScale scale,
                                      float minimumHz,
                                      float maximumHz,
                                      float minimumDb,
                                      float maximumDb)
{
    frequencyScale = scale;
    minimumFrequencyHz = minimumHz;
    maximumFrequencyHz = maximumHz;
    minimumAmplitude = minimumDb;
    maximumAmplitude = maximumDb;
    const auto axisMinimum = scale == spectrumviewer::FrequencyScale::linear
                                 ? minimumHz
                                 : std::log10 (minimumHz);
    const auto axisMaximum = scale == spectrumviewer::FrequencyScale::linear
                                 ? maximumHz
                                 : std::log10 (maximumHz);
    XYRange range { axisMinimum, axisMaximum, minimumDb, maximumDb };
    setRange (range);
    logarithmicTickPositions.clear();
    logarithmicTickLabels.clear();
    if (scale == spectrumviewer::FrequencyScale::logarithmic)
    {
        const auto firstDecade = static_cast<int> (std::ceil (axisMinimum));
        const auto lastDecade = static_cast<int> (std::floor (axisMaximum));
        for (auto exponent = firstDecade; exponent <= lastDecade; ++exponent)
        {
            const auto frequency = std::pow (10.0f, static_cast<float> (exponent));
            logarithmicTickPositions.push_back (
                (static_cast<float> (exponent) - axisMinimum)
                / (axisMaximum - axisMinimum));
            if (frequency >= 1000.0f)
                logarithmicTickLabels.emplace_back (
                    String (frequency / 1000.0f, 0) + "k");
            else
                logarithmicTickLabels.emplace_back (String (frequency, 0));
        }
    }
    xAxis->setVisible (scale == spectrumviewer::FrequencyScale::linear);
    repaint();
    xlabel (scale == spectrumviewer::FrequencyScale::linear
                ? "Frequency (Hz)"
                : "Frequency (Hz, log scale)");
}

void FrequencyPlot::paintOverChildren (Graphics& graphics)
{
    const auto drawingBounds = drawComponent->getBounds();
    const auto axisMinimum = frequencyScale == spectrumviewer::FrequencyScale::linear
                                 ? minimumFrequencyHz
                                 : std::log10 (minimumFrequencyHz);
    const auto axisMaximum = frequencyScale == spectrumviewer::FrequencyScale::linear
                                 ? maximumFrequencyHz
                                 : std::log10 (maximumFrequencyHz);
    const auto xRange = axisMaximum - axisMinimum;
    const auto yRange = maximumAmplitude - minimumAmplitude;
    if (drawingBounds.getWidth() > 0 && drawingBounds.getHeight() > 0
        && xRange > 1.0e-6f && yRange > 1.0e-6f)
    {
        graphics.saveState();
        graphics.reduceClipRegion (drawingBounds);

        for (const auto& series : lineSeries)
        {
            const auto pointCount = std::min (series.x.size(), series.y.size());
            Path path;
            auto continuing = false;
            for (std::size_t index = 0; index < pointCount; ++index)
            {
                if (! std::isfinite (series.x[index]) || ! std::isfinite (series.y[index]))
                {
                    continuing = false;
                    continue;
                }

                const auto pixelX = static_cast<float> (drawingBounds.getX())
                                    + (series.x[index] - axisMinimum) / xRange
                                          * static_cast<float> (drawingBounds.getWidth());
                const auto pixelY = static_cast<float> (drawingBounds.getBottom())
                                    - (series.y[index] - minimumAmplitude) / yRange
                                          * static_cast<float> (drawingBounds.getHeight());
                if (continuing)
                    path.lineTo (pixelX, pixelY);
                else
                {
                    path.startNewSubPath (pixelX, pixelY);
                    continuing = true;
                }
            }
            graphics.setColour (series.colour.withAlpha (series.opacity));
            graphics.strokePath (path, PathStrokeType (series.width));
        }
        graphics.restoreState();
    }

    if (frequencyScale != spectrumviewer::FrequencyScale::logarithmic)
        return;

    const auto bounds = xAxis->getBounds();
    constexpr auto padding = 10.0f;
    const auto left = static_cast<float> (bounds.getX()) + padding;
    const auto width = static_cast<float> (bounds.getWidth()) - 2.0f * padding;
    const auto axisY = static_cast<float> (bounds.getY()) + 3.0f;
    graphics.setColour (axisColour);
    graphics.setFont (FontOptions ("Inter", "Regular", 12.0f));
    graphics.drawLine (left, axisY, left + width, axisY, 2.0f);
    for (std::size_t index = 0; index < logarithmicTickPositions.size(); ++index)
    {
        const auto x = left + logarithmicTickPositions[index] * width;
        graphics.drawLine (x, axisY, x, axisY + 10.0f, 2.0f);
        graphics.drawText (logarithmicTickLabels[index],
                           static_cast<int> (x) - 30,
                           static_cast<int> (axisY) + 8,
                           60,
                           20,
                           Justification::centred,
                           false);
    }
}

std::vector<float> FrequencyPlot::transformFrequencies (
    const std::vector<float>& frequencies,
    spectrumviewer::FrequencyScale scale)
{
    if (scale == spectrumviewer::FrequencyScale::linear)
        return frequencies;
    std::vector<float> transformed (frequencies.size());
    std::transform (frequencies.begin(), frequencies.end(), transformed.begin(), [] (float frequency)
                    { return std::log10 (frequency); });
    return transformed;
}

float FrequencyPlot::frequencyAt (Point<int> point) const noexcept
{
    const auto bounds = drawComponent->getBounds();
    if (! bounds.contains (point) || bounds.getWidth() <= 0)
        return std::numeric_limits<float>::quiet_NaN();
    const auto fraction = static_cast<float> (point.x - bounds.getX())
                          / static_cast<float> (bounds.getWidth());
    if (frequencyScale == spectrumviewer::FrequencyScale::linear)
        return minimumFrequencyHz + fraction * (maximumFrequencyHz - minimumFrequencyHz);
    return std::pow (10.0f,
                     std::log10 (minimumFrequencyHz)
                         + fraction * (std::log10 (maximumFrequencyHz) - std::log10 (minimumFrequencyHz)));
}

SpectrumCanvas::SpectrumCanvas (SpectrumViewer* n,
                                SpectrumViewerEditor* controls)
    : Visualizer ((GenericProcessor*) n),
      processor (n),
      editorControls (controls),
      displayType (POWER_SPECTRUM)
{
    refreshRate = 60;

    canvasPlot = std::make_unique<CanvasPlot> (processor);

    viewport = std::make_unique<Viewport>();
    viewport->setViewedComponent (canvasPlot.get(), false);
    viewport->setScrollBarsShown (true, true);
    viewport->setScrollBarThickness (12);
    addAndMakeVisible (viewport.get());

    if (controls != nullptr)
    {
        mainOptionsBar = std::make_unique<Component> ("Spectrum display controls");
        addAndMakeVisible (mainOptionsBar.get());
        Component* mainControls[] {
            controls->displayType.get(), controls->displayLabel.get(),
            controls->frequencyRange.get(), controls->frequencyLabel.get(),
            controls->analysisProfile.get(), controls->profileLabel.get(),
            controls->frequencyScale.get(), controls->scaleLabel.get(),
            controls->amplitudeDisplay.get(), controls->amplitudeLabel.get()
        };
        for (auto* control : mainControls)
            mainOptionsBar->addAndMakeVisible (control);

        optionsDrawer = std::make_unique<Component> ("Spectrum advanced controls");
        addChildComponent (optionsDrawer.get());
        Component* drawerControls[] {
            controls->baselineDisplay.get(), controls->baselineLabel.get(),
            controls->peakEnvelope.get(),
            controls->amplitudeRangeMode.get(), controls->amplitudeRangeLabel.get(),
            controls->minimumDb.get(), controls->minimumDbLabel.get(),
            controls->maximumDb.get(), controls->maximumDbLabel.get(),
            controls->automaticRangeLabel.get(),
            controls->captureDuration.get(), controls->captureDurationLabel.get(),
            controls->captureAction.get(), controls->captureStatusLabel.get(),
            controls->comparisonMode.get(), controls->comparisonModeLabel.get(),
            controls->setReferenceAction.get(), controls->clearReferenceAction.get(),
            controls->referenceStatusLabel.get()
        };
        for (auto* control : drawerControls)
            optionsDrawer->addAndMakeVisible (control);

        showHideOptionsButton = std::make_unique<ShowHideSpectrumOptionsButton>();
        showHideOptionsButton->onClick = [this]
        {
            setOptionsDrawerOpen (showHideOptionsButton->getToggleState());
        };
        addAndMakeVisible (showHideOptionsButton.get());
        controls->updateAmplitudeRangeControls();
    }
}

void SpectrumCanvas::resized()
{
    int plotWidth, plotHeight;

    const auto controlsHeight = editorControls != nullptr
                                    ? optionsBarHeight
                                          + (optionsDrawerIsOpen
                                                 ? optionsDrawerHeight
                                                 : 0)
                                    : 0;
    viewport->setBounds (0, 0, getWidth(), std::max (0, getHeight() - controlsHeight));

    if (editorControls == nullptr)
    {
        canvasPlot->setBounds (0, 0, getWidth(), getHeight());
        return;
    }

    auto setLabelAndControl = [] (Label* label,
                                  Component* control,
                                  int x,
                                  int y,
                                  int labelWidth,
                                  int controlWidth)
    {
        label->setBounds (x, y, labelWidth, 20);
        control->setBounds (x + labelWidth, y, controlWidth, 20);
    };

    const auto barTop = getHeight() - optionsBarHeight;
    mainOptionsBar->setBounds (0, barTop, getWidth(), optionsBarHeight);
    showHideOptionsButton->setBounds (getWidth() - 90, barTop + 10, 78, 24);

    setLabelAndControl (editorControls->displayLabel.get(),
                        editorControls->displayType.get(),
                        10, 12, 52, 120);
    setLabelAndControl (editorControls->profileLabel.get(),
                        editorControls->analysisProfile.get(),
                        205, 12, 58, 100);
    setLabelAndControl (editorControls->frequencyLabel.get(),
                        editorControls->frequencyRange.get(),
                        385, 12, 100, 130);

    setLabelAndControl (editorControls->scaleLabel.get(),
                        editorControls->frequencyScale.get(),
                        640, 12, 95, 90);
    setLabelAndControl (editorControls->amplitudeLabel.get(),
                        editorControls->amplitudeDisplay.get(),
                        850, 12, 50, 80);

    if (optionsDrawerIsOpen)
    {
        const auto drawerTop = barTop - optionsDrawerHeight;
        optionsDrawer->setBounds (0, drawerTop, getWidth(), optionsDrawerHeight);

        setLabelAndControl (editorControls->amplitudeRangeLabel.get(),
                            editorControls->amplitudeRangeMode.get(),
                            10, 10, 72, 90);
        setLabelAndControl (editorControls->maximumDbLabel.get(),
                            editorControls->maximumDb.get(),
                            195, 10, 82, 110);
        setLabelAndControl (editorControls->minimumDbLabel.get(),
                            editorControls->minimumDb.get(),
                            410, 10, 82, 110);
        editorControls->automaticRangeLabel->setBounds (195, 10, 307, 20);

        setLabelAndControl (editorControls->captureDurationLabel.get(),
                            editorControls->captureDuration.get(),
                            645, 10, 100, 90);
        editorControls->captureAction->setBounds (850, 10, 95, 20);
        editorControls->captureStatusLabel->setBounds (955, 10, 255, 20);

        setLabelAndControl (editorControls->comparisonModeLabel.get(),
                            editorControls->comparisonMode.get(),
                            10, 50, 82, 100);
        setLabelAndControl (editorControls->baselineLabel.get(),
                            editorControls->baselineDisplay.get(),
                            210, 50, 82, 100);
        editorControls->peakEnvelope->setBounds (410, 50, 125, 20);
        editorControls->setReferenceAction->setBounds (545, 50, 105, 20);
        editorControls->clearReferenceAction->setBounds (660, 50, 90, 20);
        editorControls->referenceStatusLabel->setBounds (760, 50, 360, 20);
    }

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
    if (editorControls == nullptr)
        return;

    g.setColour (findColour (ThemeColours::componentBackground));
    g.fillRect (mainOptionsBar->getBounds());
    if (optionsDrawerIsOpen)
    {
        g.fillRect (optionsDrawer->getBounds());
        g.setColour (findColour (ThemeColours::controlPanelText).withAlpha (0.25f));
        g.drawHorizontalLine (optionsDrawer->getY(), 0.0f,
                              static_cast<float> (getWidth()));
    }
}

void SpectrumCanvas::setOptionsDrawerOpen (bool shouldBeOpen)
{
    if (editorControls == nullptr)
        return;

    optionsDrawerIsOpen = shouldBeOpen;
    optionsDrawer->setVisible (shouldBeOpen);
    showHideOptionsButton->setToggleState (shouldBeOpen, dontSendNotification);
    resized();
    repaint();
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
    const auto unavailable = ! processor->hasActiveAnalysis()
                             && (readiness == SpectrumAnalysisReadiness::preparing
                                 || readiness == SpectrumAnalysisReadiness::warmingUp
                                 || readiness == SpectrumAnalysisReadiness::configurationFailed);
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

    bool receivedFrame = false;

    processor->consumeLatestSpectrumFrame ([&] (const spectrumviewer::SpectrumFrameFifo::FrameView& frame)
                                           {
        canvasPlot->beginSpectrumFrame (
            frame.configurationGeneration,
            frame.sequence,
            frame.numChannels,
            static_cast<double> (frame.descriptor.hopSampleCount)
                / frame.descriptor.sampleRateHz,
            frame.sourceStreamId,
            frame.getSourceChannelIndices());
        receivedFrame = true;
        for (std::size_t channel = 0; channel < frame.numChannels; ++channel)
        {
            canvasPlot->updatePowerSpectrum (frame.getChannelData (channel),
                                             frame.getChannelPeakData (channel),
                                             frame.numBins,
                                             frame.frequenciesHz,
                                             frame.getSourceChannelUnit (channel),
                                             frame.frequencyScale,
                                             frame.minimumFrequencyHz,
                                             frame.maximumFrequencyHz,
                                             static_cast<int> (channel),
                                             frame.getChannelBaselineData (channel),
                                             frame.getChannelComparisonData (channel),
                                             frame.comparison);
        } });

    if (receivedFrame)
    {
        if (displayType == POWER_SPECTRUM)
            canvasPlot->plotPowerSpectrum (true);
        else
            canvasPlot->drawSpectrogram();
    }
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
    plt = std::make_unique<FrequencyPlot>();
    plt->title ("POWER SPECTRUM");
    const auto initialAmplitudeRange = amplitudeRange.getCurrentRange();
    XYRange range { 0, 1000,
                    initialAmplitudeRange.minimum,
                    initialAmplitudeRange.maximum };
    plt->setRange (range);
    plt->xlabel ("Frequency (Hz)");
    plt->ylabel ("PSD (dB re native unit²/Hz)");
    plt->setBackgroundColour (Colour (45, 45, 45));
    plt->setGridColour (Colour (100, 100, 100));
    plt->setInteractive (InteractivePlotMode::OFF);
    addAndMakeVisible (plt.get());
    plt->addMouseListener (this, true);

    cursorLabel = std::make_unique<Label> ("SpectrumCursor", "");
    cursorLabel->setFont (FontOptions ("Inter", "Regular", 12.0f));
    addAndMakeVisible (cursorLabel.get());

    clearButton = std::make_unique<UtilityButton> ("Clear");
    clearButton->addListener (this);
    addAndMakeVisible (clearButton.get());

    activeChannels = processor->getActiveChans();

    spectrogramImg = std::make_unique<Image> (Image::RGB, 1000, 1000, true, SoftwareImageType());
    setOpaque (true);

    currPower.resize (MAX_CHANS);
    currPeakPower.resize (MAX_CHANS);
    currBaselineDb.resize (MAX_CHANS);
    displayedPower.resize (MAX_CHANS);
    displayedPeakPower.resize (MAX_CHANS);
    currLinearPower.resize (MAX_CHANS);
    currLinearPeakPower.resize (MAX_CHANS);
    currComparison.resize (MAX_CHANS);
    channelUnits.resize (MAX_CHANS);

    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].clear();
        currPeakPower[ch].clear();
        currBaselineDb[ch].clear();
        displayedPower[ch].clear();
        displayedPeakPower[ch].clear();
        currLinearPower[ch].clear();
        currLinearPeakPower[ch].clear();
        currComparison[ch].clear();
    }

    for (int i = 0; i < nFreqs; i++)
    {
        xvalues.push_back (i * freqStep);
    }
}

void CanvasPlot::resized()
{
    plt->setBounds (20, 30, getWidth() - legendWidth - 40, getHeight() - 50);
    processor->setDisplayColumnCount (
        static_cast<std::size_t> (std::max (1, plt->getDrawingWidth())));
    clearButton->setBounds (plt->getRight() - 80, plt->getY(), 60, 20);
    cursorLabel->setBounds (plt->getX() + 60, plt->getY(), 560, 20);
}

void CanvasPlot::lookAndFeelChanged()
{
    plt->setBackgroundColour (findColour (ThemeColours::componentBackground));
    plt->setGridColour (findColour (ThemeColours::controlPanelText).withAlpha (0.5f));
    plt->setAxisColour (findColour (ThemeColours::controlPanelText));

    chanColors[0] = findColour (ThemeColours::defaultText);
    plotPowerSpectrum();
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

    const auto amplitude = amplitudeRange.getCurrentRange();
    XYRange range { (float) freqStart_, (float) freqEnd_,
                    amplitude.minimum, amplitude.maximum };
    plt->setRange (range);

    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currBaselineDb[ch].clear();
        displayedPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        displayedPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currComparison[ch].clear();
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

void CanvasPlot::plotPowerSpectrum (bool updateAutomaticRange)
{
    plt->clear();
    updateAmplitudeAxisLabel();

    const auto plotFrequencies = plt->transformFrequencies (xvalues, frequencyScale);
    const auto showingDelta = comparisonStatus.mode
                                  == spectrumviewer::SpectrumComparisonMode::deltaDb
                              && comparisonStatus.hasComparisonData();
    const auto showingAperiodic = ! comparisonStatus.hasComparisonData()
                                  && aperiodicDisplayMode
                                         == spectrumviewer::AperiodicDisplayMode::show;
    if (showingDelta)
        plt->plot (plotFrequencies,
                   std::vector<float> (xvalues.size(), 0.0f),
                   findColour (ThemeColours::controlPanelText),
                   1.0f,
                   0.5f);
    for (int i = 0; i < activeChannels.size(); i++)
    {
        if (showingDelta)
            plt->plot (plotFrequencies, currComparison[i], chanColors[i], 1.5f);
        else
        {
            if (comparisonStatus.mode == spectrumviewer::SpectrumComparisonMode::overlay
                && comparisonStatus.hasComparisonData())
                plt->plot (plotFrequencies, currComparison[i], chanColors[i], 1.0f, 0.55f);
            if (showingAperiodic && currBaselineDb[i].size() == xvalues.size())
                plt->plot (plotFrequencies, currBaselineDb[i], chanColors[i], 2.0f, 0.55f);
            if (peakEnvelopeVisible)
                plt->plot (plotFrequencies, displayedPeakPower[i], chanColors[i], 1.0f, 0.35f);
            plt->plot (plotFrequencies, displayedPower[i], chanColors[i], 1.5f);
        }
    }

    if (! xvalues.empty())
    {
        if (amplitudeUnitsChanged)
        {
            amplitudeRange.resetAutomatic();
            amplitudeUnitsChanged = false;
        }
        const auto range = showingDelta
                               ? spectrumviewer::DecibelRange { -12.0f, 12.0f }
                               : updateAutomaticRange
                                     ? amplitudeRange.update (displayedPower,
                                                              displayedPeakPower,
                                                              frameChannelCount,
                                                              pendingRangeElapsedSeconds)
                                     : amplitudeRange.getCurrentRange();
        if (updateAutomaticRange)
            pendingRangeElapsedSeconds = 0.0;
        plt->setFrequencyAxis (frequencyScale,
                               displayMinimumFrequencyHz,
                               displayMaximumFrequencyHz,
                               range.minimum,
                               range.maximum);
    }
}

void CanvasPlot::updatePowerSpectrum (const float* meanPsd,
                                      const float* peakPsd,
                                      std::size_t valueCount,
                                      const float* frequenciesHz,
                                      const String& unit,
                                      spectrumviewer::FrequencyScale scale,
                                      double minimumFrequencyHz,
                                      double maximumFrequencyHz,
                                      int channelIndex,
                                      const float* baselineDb,
                                      const float* comparisonData,
                                      spectrumviewer::SpectrumComparisonFrameStatus comparison)
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (currPower.size())
        || meanPsd == nullptr || peakPsd == nullptr || frequenciesHz == nullptr)
        return;
    frequencyScale = scale;
    comparisonStatus = comparison;
    displayMinimumFrequencyHz = static_cast<float> (minimumFrequencyHz);
    displayMaximumFrequencyHz = static_cast<float> (maximumFrequencyHz);
    if (channelIndex == 0)
        xvalues.assign (frequenciesHz, frequenciesHz + valueCount);
    auto& channelUnit = channelUnits[static_cast<std::size_t> (channelIndex)];
    if (channelUnit != unit)
    {
        amplitudeUnitsChanged = true;
        repaint();
    }
    channelUnit = unit;
    auto& destination = currPower[static_cast<std::size_t> (channelIndex)];
    auto& peakDestination = currPeakPower[static_cast<std::size_t> (channelIndex)];
    auto& baselineDestination = currBaselineDb[static_cast<std::size_t> (channelIndex)];
    auto& linearDestination = currLinearPower[static_cast<std::size_t> (channelIndex)];
    auto& linearPeakDestination = currLinearPeakPower[static_cast<std::size_t> (channelIndex)];
    auto& comparisonDestination = currComparison[static_cast<std::size_t> (channelIndex)];
    destination.resize (valueCount);
    peakDestination.resize (valueCount);
    linearDestination.assign (meanPsd, meanPsd + valueCount);
    linearPeakDestination.assign (peakPsd, peakPsd + valueCount);
    comparisonDestination.clear();
    baselineDestination.clear();
    if (baselineDb != nullptr)
        baselineDestination.assign (baselineDb, baselineDb + valueCount);
    if (comparison.hasComparisonData() && comparisonData != nullptr)
    {
        comparisonDestination.resize (valueCount);
        if (comparison.mode == spectrumviewer::SpectrumComparisonMode::deltaDb)
            std::copy (comparisonData,
                       comparisonData + valueCount,
                       comparisonDestination.begin());
        else
            for (std::size_t n = 0; n < valueCount; ++n)
                comparisonDestination[n] = std::isfinite (comparisonData[n])
                                                   && comparisonData[n] > 0.0f
                                               ? 10.0f * std::log10 (comparisonData[n])
                                               : std::numeric_limits<float>::quiet_NaN();
    }
    for (std::size_t n = 0; n < valueCount; ++n)
    {
        const auto power = meanPsd[n];
        if (std::isfinite (power) && power > 0.0f)
            destination[n] = 10.0f * std::log10 (power);
        else
            destination[n] = std::numeric_limits<float>::quiet_NaN();
        const auto peak = peakPsd[n];
        peakDestination[n] = std::isfinite (peak) && peak > 0.0f
                                 ? 10.0f * std::log10 (peak)
                                 : std::numeric_limits<float>::quiet_NaN();
    }
    rebuildDisplayedTraces();
}

void CanvasPlot::setAmplitudeDisplay (SpectrumAmplitudeDisplay display)
{
    amplitudeDisplay = display;
    updateAmplitudeAxisLabel();
}

void CanvasPlot::setAperiodicDisplayMode (
    spectrumviewer::AperiodicDisplayMode mode)
{
    aperiodicDisplayMode = mode;
    rebuildDisplayedTraces();
    amplitudeRange.resetAutomatic();
    plotPowerSpectrum (hasFrameTiming);
}

void CanvasPlot::setPeakEnvelopeVisible (bool shouldBeVisible)
{
    if (peakEnvelopeVisible == shouldBeVisible)
        return;
    peakEnvelopeVisible = shouldBeVisible;
    amplitudeRange.resetAutomatic();
    plotPowerSpectrum (true);
}

void CanvasPlot::rebuildDisplayedTraces()
{
    const auto remove = aperiodicDisplayMode
                            == spectrumviewer::AperiodicDisplayMode::remove
                        && ! comparisonStatus.hasComparisonData();
    for (std::size_t channel = 0; channel < currPower.size(); ++channel)
    {
        displayedPower[channel] = currPower[channel];
        displayedPeakPower[channel] = currPeakPower[channel];
        if (! remove || currBaselineDb[channel].size() != currPower[channel].size())
            continue;
        for (std::size_t bin = 0; bin < currPower[channel].size(); ++bin)
        {
            displayedPower[channel][bin] -= currBaselineDb[channel][bin];
            displayedPeakPower[channel][bin] -= currBaselineDb[channel][bin];
        }
    }
}

void CanvasPlot::setAmplitudeRangeMode (spectrumviewer::AmplitudeRangeMode mode)
{
    amplitudeRange.setMode (mode);
    plotPowerSpectrum (mode == spectrumviewer::AmplitudeRangeMode::automatic
                       && hasFrameTiming);
}

bool CanvasPlot::setFixedAmplitudeRange (float minimumDb, float maximumDb)
{
    if (! amplitudeRange.setFixedRange (minimumDb, maximumDb))
        return false;
    if (amplitudeRange.getMode() == spectrumviewer::AmplitudeRangeMode::fixed)
        plotPowerSpectrum();
    return true;
}

spectrumviewer::AmplitudeRangeMode CanvasPlot::getAmplitudeRangeMode() const noexcept
{
    return amplitudeRange.getMode();
}

spectrumviewer::DecibelRange CanvasPlot::getAmplitudeRange() const noexcept
{
    return amplitudeRange.getCurrentRange();
}

bool CanvasPlot::hasAutomaticAmplitudeRange() const noexcept
{
    return amplitudeRange.hasAutomaticRange();
}

void CanvasPlot::beginSpectrumFrame (std::uint64_t configurationGeneration,
                                     std::uint64_t sequence,
                                     std::size_t channelCount,
                                     double hopDurationSeconds,
                                     std::uint16_t sourceStreamId,
    const int* sourceChannelIndices)
{
    frameChannelCount = std::min (channelCount, currPower.size());
    if (sourceChannelIndices != nullptr)
    {
        Array<int> frameChannels;
        for (std::size_t channel = 0; channel < frameChannelCount; ++channel)
            frameChannels.add (sourceChannelIndices[channel]);
        const auto legendChanged = activeStreamId != sourceStreamId
                                   || activeChannels != frameChannels;
        activeChannels = std::move (frameChannels);
        activeStreamId = sourceStreamId;
        if (legendChanged)
            repaint();
    }
    auto elapsedFrames = std::uint64_t { 1 };
    if (hasFrameTiming && configurationGeneration == lastConfigurationGeneration)
        elapsedFrames = sequence > lastFrameSequence
                            ? sequence - lastFrameSequence
                            : 0;
    pendingRangeElapsedSeconds = static_cast<double> (elapsedFrames)
                                 * hopDurationSeconds;
    lastConfigurationGeneration = configurationGeneration;
    lastFrameSequence = sequence;
    hasFrameTiming = true;
}

void CanvasPlot::updateAmplitudeAxisLabel()
{
    if (comparisonStatus.mode == spectrumviewer::SpectrumComparisonMode::deltaDb
        && comparisonStatus.hasComparisonData())
    {
        plt->ylabel ("Difference (dB re reference)");
        return;
    }
    if (aperiodicDisplayMode == spectrumviewer::AperiodicDisplayMode::remove
        && ! comparisonStatus.hasComparisonData())
    {
        plt->ylabel ("Spectral residual (dB above background)");
        return;
    }
    String commonUnit;
    auto hasCommonUnit = false;
    for (int index = 0; index < activeChannels.size(); ++index)
    {
        const auto& unit = channelUnits[static_cast<std::size_t> (index)];
        if (unit.isEmpty())
        {
            commonUnit.clear();
            hasCommonUnit = false;
            break;
        }
        if (! hasCommonUnit)
        {
            commonUnit = unit;
            hasCommonUnit = true;
        }
        else if (unit != commonUnit)
        {
            commonUnit.clear();
            hasCommonUnit = false;
            break;
        }
    }

    if (! hasCommonUnit)
        commonUnit = "native unit";
    plt->ylabel (amplitudeDisplay == SpectrumAmplitudeDisplay::psd
                     ? "PSD (dB re " + commonUnit + "²/Hz)"
                     : "ASD (dB re " + commonUnit + "/√Hz)");
}

void CanvasPlot::mouseMove (const MouseEvent& event)
{
    if (xvalues.empty() || currLinearPower.empty())
        return;
    const auto frequency = plt->frequencyAt (event.getEventRelativeTo (plt.get()).getPosition());
    if (! std::isfinite (frequency))
        return;
    const auto iterator = std::lower_bound (xvalues.begin(), xvalues.end(), frequency);
    auto index = static_cast<std::size_t> (
        std::min<std::ptrdiff_t> (std::distance (xvalues.begin(), iterator),
                                  static_cast<std::ptrdiff_t> (xvalues.size() - 1)));
    if (index > 0 && std::abs (frequency - xvalues[index - 1]) < std::abs (xvalues[index] - frequency))
        --index;
    if (currLinearPower[0].size() <= index || currLinearPeakPower[0].size() <= index)
        return;
    const auto power = currLinearPower[0][index];
    const auto peakPower = currLinearPeakPower[0][index];
    if (! std::isfinite (power) || power < 0.0f
        || ! std::isfinite (peakPower) || peakPower < 0.0f)
    {
        cursorLabel->setText ({}, dontSendNotification);
        return;
    }
    const auto shown = amplitudeDisplay == SpectrumAmplitudeDisplay::psd
                           ? power
                           : std::sqrt (power);
    const auto shownPeak = amplitudeDisplay == SpectrumAmplitudeDisplay::psd
                               ? peakPower
                               : std::sqrt (peakPower);
    const auto unit = channelUnits[0].isEmpty() ? String ("unit") : channelUnits[0];
    const auto channel = activeChannels.isEmpty()
                             ? String ("Channel")
                             : hasFrameTiming
                                   ? processor->getChanName (activeStreamId,
                                                             activeChannels[0])
                                   : processor->getChanName (activeChannels[0]);
    if (comparisonStatus.hasComparisonData()
        && currComparison[0].size() > index && power > 0.0f
        && std::isfinite (currComparison[0][index]))
    {
        if (comparisonStatus.mode == spectrumviewer::SpectrumComparisonMode::deltaDb)
        {
            const auto delta = currComparison[0][index];
            const auto referencePower = std::isfinite (delta)
                                            ? power / std::pow (10.0f, delta / 10.0f)
                                            : std::numeric_limits<float>::quiet_NaN();
            cursorLabel->setText (channel + ": " + String (xvalues[index], 2)
                                      + " Hz, current " + String (10.0f * std::log10 (power), 2)
                                      + " dB, reference "
                                      + String (10.0f * std::log10 (referencePower), 2)
                                      + " dB, delta " + String (delta, 2) + " dB",
                                  dontSendNotification);
            return;
        }
        if (comparisonStatus.mode == spectrumviewer::SpectrumComparisonMode::overlay)
        {
            const auto referenceDb = currComparison[0][index];
            cursorLabel->setText (channel + ": " + String (xvalues[index], 2)
                                      + " Hz, current " + String (10.0f * std::log10 (power), 2)
                                      + " dB, reference " + String (referenceDb, 2) + " dB",
                                  dontSendNotification);
            return;
        }
    }
    if (aperiodicDisplayMode != spectrumviewer::AperiodicDisplayMode::off
        && ! comparisonStatus.hasComparisonData()
        && currBaselineDb[0].size() > index
        && std::isfinite (currBaselineDb[0][index]))
    {
        const auto rawDb = 10.0f * std::log10 (power);
        const auto baselineDb = currBaselineDb[0][index];
        cursorLabel->setText (channel + ": " + String (xvalues[index], 2)
                                  + " Hz, raw " + String (rawDb, 2)
                                  + " dB, background " + String (baselineDb, 2)
                                  + " dB, residual "
                                  + String (rawDb - baselineDb, 2) + " dB",
                              dontSendNotification);
        return;
    }
    cursorLabel->setText (channel + ": " + String (xvalues[index], 2)
                              + " Hz, mean " + String (shown, 4)
                              + (peakEnvelopeVisible
                                     ? ", peak " + String (shownPeak, 4)
                                     : String())
                              + " " + unit
                              + (amplitudeDisplay == SpectrumAmplitudeDisplay::psd
                                     ? String ("^2/Hz")
                                     : String ("/sqrt(Hz)")),
                          dontSendNotification);
}

void CanvasPlot::drawSpectrogram()
{
    if (frameChannelCount == 0 || displayedPower.empty() || displayedPower[0].empty())
        return;

    if (amplitudeUnitsChanged)
    {
        amplitudeRange.resetAutomatic();
        amplitudeUnitsChanged = false;
    }
    const auto colourRange = amplitudeRange.getMode()
                                     == spectrumviewer::AmplitudeRangeMode::automatic
                                 ? amplitudeRange.update (displayedPower,
                                                          displayedPeakPower,
                                                          1,
                                                          pendingRangeElapsedSeconds)
                                 : amplitudeRange.getCurrentRange();
    pendingRangeElapsedSeconds = 0.0;
    const auto colourSpan = colourRange.maximum - colourRange.minimum;
    if (! std::isfinite (colourSpan) || colourSpan <= 0.0f)
        return;

    auto imageWidth = spectrogramImg->getWidth() - 1;
    auto imageHeight = spectrogramImg->getHeight();

    // first, shuffle our image rightwards by 1 pixel..
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth, imageHeight);

    ColourGradient colours (Colour::fromRGB (68, 1, 84), 0.0f, 0.0f,
                            Colour::fromRGB (253, 231, 37), 1.0f, 0.0f, false);
    colours.addColour (0.33, Colour::fromRGB (49, 104, 142));
    colours.addColour (0.66, Colour::fromRGB (53, 183, 121));
    const auto& channelDb = displayedPower[0];

    for (auto y = 0; y < imageHeight - 1; ++y)
    {
        auto skewedProportionY = 1.0f - (float) y / (float) imageHeight;
        auto dataIndex = (size_t) jlimit (0, (int) (channelDb.size() - 1), (int) (skewedProportionY * (channelDb.size() - 1)));
        const auto valueDb = channelDb[dataIndex];
        const auto level = std::isfinite (valueDb)
                               ? jlimit (0.0f, 1.0f,
                                         (valueDb - colourRange.minimum) / colourSpan)
                               : 0.0f;
        spectrogramImg->setPixelAt (0, y, colours.getColourAtPosition (level));
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
            String chan = hasFrameTiming
                              ? processor->getChanName (activeStreamId,
                                                        activeChannels[i])
                              : processor->getChanName (activeChannels[i]);
            if (! channelUnits[static_cast<std::size_t> (i)].isEmpty())
                chan += " [" + channelUnits[static_cast<std::size_t> (i)] + "]";
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

        std::vector<float> tickFrequencies;
        if (frequencyScale == spectrumviewer::FrequencyScale::linear)
        {
            for (int tick = 0; tick <= 10; ++tick)
                tickFrequencies.push_back (
                    displayMinimumFrequencyHz
                    + static_cast<float> (tick) / 10.0f
                          * (displayMaximumFrequencyHz - displayMinimumFrequencyHz));
        }
        else
        {
            const auto firstDecade = static_cast<int> (
                std::ceil (std::log10 (displayMinimumFrequencyHz)));
            const auto lastDecade = static_cast<int> (
                std::floor (std::log10 (displayMaximumFrequencyHz)));
            for (auto exponent = firstDecade; exponent <= lastDecade; ++exponent)
                tickFrequencies.push_back (
                    std::pow (10.0f, static_cast<float> (exponent)));
        }

        for (const auto frequency : tickFrequencies)
        {
            const auto fraction = frequencyScale
                                           == spectrumviewer::FrequencyScale::linear
                                      ? (frequency - displayMinimumFrequencyHz)
                                            / (displayMaximumFrequencyHz
                                               - displayMinimumFrequencyHz)
                                      : (std::log10 (frequency)
                                         - std::log10 (displayMinimumFrequencyHz))
                                            / (std::log10 (displayMaximumFrequencyHz)
                                               - std::log10 (displayMinimumFrequencyHz));
            const auto ytickloc = static_cast<float> (h - padding)
                                  - fraction * static_cast<float> (h - 2 * padding);
            g.drawLine (w - 13, ytickloc, w - 3, ytickloc, 2.0);

            const auto yTick = frequency >= 1000.0f
                                   ? String (frequency / 1000.0f, 0) + "k"
                                   : String (frequency, 0);

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
        currPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currBaselineDb[ch].clear();
        displayedPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        displayedPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currComparison[ch].clear();
    }

    comparisonStatus = {};

    spectrogramImg->clear (spectrogramImg->getBounds());
    plt->clear();
}
