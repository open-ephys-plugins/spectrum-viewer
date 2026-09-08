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
#include <limits>
#include <utility>

namespace
{
class BatchedXYLine final : public XYLine
{
public:
    BatchedXYLine (std::vector<float> xValues, std::vector<float> yValues)
        : XYLine (std::move (xValues), std::move (yValues))
    {
    }

    void draw (Graphics& graphics, XYRange& visibleRange, int plotWidth, int plotHeight) override
    {
        if (type != PlotType::LINE)
        {
            XYLine::draw (graphics, visibleRange, plotWidth, plotHeight);
            return;
        }

        const auto xRange = visibleRange.xmax - visibleRange.xmin;
        const auto yRange = visibleRange.ymax - visibleRange.ymin;
        const auto pointCount = std::min (x.size(), y.size());
        if (pointCount < 2 || xRange < 1.0e-6f || yRange < 1.0e-6f)
            return;

        Path path;
        auto continuing = false;
        for (std::size_t index = 0; index < pointCount; ++index)
        {
            if (! std::isfinite (x[index]) || ! std::isfinite (y[index]))
            {
                continuing = false;
                continue;
            }

            const auto pixelX = (x[index] - visibleRange.xmin) / xRange
                                * static_cast<float> (plotWidth);
            const auto pixelY = static_cast<float> (plotHeight)
                                - (y[index] - visibleRange.ymin) / yRange
                                      * static_cast<float> (plotHeight);
            if (continuing)
                path.lineTo (pixelX, pixelY);
            else
            {
                path.startNewSubPath (pixelX, pixelY);
                continuing = true;
            }
        }

        graphics.setColour (colour.withAlpha (opacity));
        graphics.strokePath (path, PathStrokeType (width));
    }
};
} // namespace

void FrequencyPlot::plot (std::vector<float> x,
                          std::vector<float> y,
                          Colour colour,
                          float width,
                          float opacity,
                          PlotType type)
{
    auto* line = new BatchedXYLine (std::move (x), std::move (y));
    line->setColour (colour);
    line->setWidth (width);
    line->setOpacity (opacity);
    line->setType (type);
    drawComponent->add (line);
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
    const auto axisMinimum = scale == spectrumviewer::FrequencyScale::linear
                                 ? minimumHz
                                 : std::log10 (minimumHz);
    const auto axisMaximum = scale == spectrumviewer::FrequencyScale::linear
                                 ? maximumHz
                                 : std::log10 (maximumHz);
    XYRange range { axisMinimum, axisMaximum, minimumDb, maximumDb };
    setRange (range);
    xlabel (scale == spectrumviewer::FrequencyScale::linear
                ? "Frequency (Hz)"
                : "log10 Frequency (Hz)");
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

SpectrumCanvas::SpectrumCanvas (SpectrumViewer* n)
    : Visualizer ((GenericProcessor*) n), processor (n), displayType (POWER_SPECTRUM)
{
    refreshRate = 60;

    canvasPlot = std::make_unique<CanvasPlot> (processor);

    viewport = std::make_unique<Viewport>();
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

    bool needsRedraw = false;

    processor->consumeLatestSpectrumFrame ([&] (const spectrumviewer::SpectrumFrameFifo::FrameView& frame)
                                           {
        canvasPlot->beginSpectrumFrame (
            frame.configurationGeneration,
            frame.sequence,
            frame.numChannels,
            static_cast<double> (frame.descriptor.hopSampleCount)
                / frame.descriptor.sampleRateHz);
        for (std::size_t channel = 0; channel < frame.numChannels; ++channel)
        {
            if (displayType == POWER_SPECTRUM)
            {
                needsRedraw = true;
                canvasPlot->updatePowerSpectrum (frame.getChannelData (channel),
                                                 frame.getChannelPeakData (channel),
                                                 frame.numBins,
                                                 frame.frequenciesHz,
                                                 frame.getSourceChannelUnit (channel),
                                                 frame.frequencyScale,
                                                 frame.minimumFrequencyHz,
                                                 frame.maximumFrequencyHz,
                                                 static_cast<int> (channel));
            }
            else if (channel == 0)
                canvasPlot->drawSpectrogram (
                    std::vector<float> (frame.getChannelData (0),
                                        frame.getChannelData (0) + frame.numBins));
        } });

    if (needsRedraw)
        canvasPlot->plotPowerSpectrum (true);
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
    plt->ylabel ("PSD (dB re native unit^2/Hz)");
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
    currLinearPower.resize (MAX_CHANS);
    currLinearPeakPower.resize (MAX_CHANS);
    channelUnits.resize (MAX_CHANS);

    for (int ch = 0; ch < MAX_CHANS; ch++)
    {
        currPower[ch].clear();
        currPeakPower[ch].clear();
        currLinearPower[ch].clear();
        currLinearPeakPower[ch].clear();
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
    clearButton->setBounds (plt->getRight() - 80, plt->getBottom() - 90, 60, 20);
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
        currLinearPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
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
    for (int i = 0; i < activeChannels.size(); i++)
    {
        plt->plot (plotFrequencies, currPeakPower[i], chanColors[i], 1.0f, 0.35f);
        plt->plot (plotFrequencies, currPower[i], chanColors[i], 1.5f);
    }

    if (! xvalues.empty())
    {
        if (amplitudeUnitsChanged)
        {
            amplitudeRange.resetAutomatic();
            amplitudeUnitsChanged = false;
        }
        const auto range = updateAutomaticRange
                               ? amplitudeRange.update (currPower,
                                                        currPeakPower,
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
                                      int channelIndex)
{
    if (channelIndex < 0 || channelIndex >= static_cast<int> (currPower.size())
        || meanPsd == nullptr || peakPsd == nullptr || frequenciesHz == nullptr)
        return;
    frequencyScale = scale;
    displayMinimumFrequencyHz = static_cast<float> (minimumFrequencyHz);
    displayMaximumFrequencyHz = static_cast<float> (maximumFrequencyHz);
    if (channelIndex == 0)
        xvalues.assign (frequenciesHz, frequenciesHz + valueCount);
    auto& channelUnit = channelUnits[static_cast<std::size_t> (channelIndex)];
    if (channelUnit != unit)
        amplitudeUnitsChanged = true;
    channelUnit = unit;
    auto& destination = currPower[static_cast<std::size_t> (channelIndex)];
    auto& peakDestination = currPeakPower[static_cast<std::size_t> (channelIndex)];
    auto& linearDestination = currLinearPower[static_cast<std::size_t> (channelIndex)];
    auto& linearPeakDestination = currLinearPeakPower[static_cast<std::size_t> (channelIndex)];
    destination.resize (valueCount);
    peakDestination.resize (valueCount);
    linearDestination.assign (meanPsd, meanPsd + valueCount);
    linearPeakDestination.assign (peakPsd, peakPsd + valueCount);
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
}

void CanvasPlot::setAmplitudeDisplay (SpectrumAmplitudeDisplay display)
{
    amplitudeDisplay = display;
    updateAmplitudeAxisLabel();
}

void CanvasPlot::setAmplitudeRangeMode (spectrumviewer::AmplitudeRangeMode mode)
{
    amplitudeRange.setMode (mode);
    plotPowerSpectrum (mode == spectrumviewer::AmplitudeRangeMode::automatic);
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
                                     double hopDurationSeconds)
{
    frameChannelCount = std::min (channelCount, currPower.size());
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
                     ? "PSD (dB re " + commonUnit + "^2/Hz)"
                     : "ASD (dB re " + commonUnit + "/sqrt(Hz))");
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
                             : processor->getChanName (activeChannels[0]);
    cursorLabel->setText (channel + ": " + String (xvalues[index], 2)
                              + " Hz, mean " + String (shown, 4)
                              + ", peak " + String (shownPeak, 4) + " " + unit
                              + (amplitudeDisplay == SpectrumAmplitudeDisplay::psd
                                     ? String ("^2/Hz")
                                     : String ("/sqrt(Hz)")),
                          dontSendNotification);
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
        currPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
        currLinearPeakPower[ch].assign (static_cast<std::size_t> (std::max (0, nFreqs)), 0.0f);
    }

    spectrogramImg->clear (spectrogramImg->getBounds());
    plt->clear();
}
