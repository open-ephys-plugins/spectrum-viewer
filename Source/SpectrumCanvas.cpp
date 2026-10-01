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

    void paintButton (Graphics& g, bool isMouseOver, bool) override
    {
        g.setColour (findColour (ThemeColours::defaultText)
                                .withAlpha (isMouseOver ? 1.0f : 0.85f));
        g.setFont (FontOptions ("Inter", "Regular", 13.0f));
        g.drawText ("Options", 0, 0, getWidth() - 18, getHeight(),
                    Justification::centredRight, false);

        const auto centreX = static_cast<float> (getWidth() - 9);
        const auto centreY = static_cast<float> (getHeight()) * 0.5f;
        Path arrow;
        if (getToggleState())
            arrow.addTriangle (centreX, centreY - 4.5f,
                               centreX - 5.0f, centreY + 3.5f,
                               centreX + 5.0f, centreY + 3.5f);
        else
            arrow.addTriangle (centreX - 3.5f, centreY - 5.0f,
                               centreX + 4.5f, centreY,
                               centreX - 3.5f, centreY + 5.0f);
        g.fillPath (arrow.createPathWithRoundedCorners (2.0f));
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

    auto& series = nextLineSeries();
    series.x = std::move (x);
    series.y = std::move (y);
    series.colour = colour;
    series.width = width;
    series.opacity = opacity;
    repaint (drawComponent->getBounds());
}

void FrequencyPlot::plotLine (const std::vector<float>& x,
                              const std::vector<float>& y,
                              Colour colour,
                              float width,
                              float opacity)
{
    auto& series = nextLineSeries();
    // assign() keeps the existing capacity, which is what makes a redraw of
    // the same number of points allocation-free.
    series.x.assign (x.begin(), x.end());
    series.y.assign (y.begin(), y.end());
    series.colour = colour;
    series.width = width;
    series.opacity = opacity;
    repaint (drawComponent->getBounds());
}

FrequencyPlot::LineSeries& FrequencyPlot::nextLineSeries()
{
    if (lineSeriesCount == lineSeries.size())
        lineSeries.emplace_back();
    return lineSeries[lineSeriesCount++];
}

void FrequencyPlot::clear()
{
    lineSeriesCount = 0;
    InteractivePlot::clear();
}

void FrequencyPlot::setFrequencyAxis (spectrumviewer::FrequencyScale scale,
                                      float minimumHz,
                                      float maximumHz,
                                      float minimumDb,
                                      float maximumDb)
{
    // Called every frame, and usually only the amplitude range has moved. The
    // tick labels and axis title below allocate, so rebuild them only when
    // the frequency axis itself changes.
    const auto frequencyAxisChanged = ! frequencyAxisValid
                                      || scale != frequencyScale
                                      || minimumHz != minimumFrequencyHz
                                      || maximumHz != maximumFrequencyHz;
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
    repaint();
    if (! frequencyAxisChanged)
        return;

    frequencyAxisValid = true;
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

        for (std::size_t seriesIndex = 0; seriesIndex < lineSeriesCount; ++seriesIndex)
        {
            const auto& series = lineSeries[seriesIndex];
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
    std::vector<float> transformed;
    transformFrequencies (frequencies, scale, transformed);
    return transformed;
}

void FrequencyPlot::transformFrequencies (const std::vector<float>& frequencies,
                                          spectrumviewer::FrequencyScale scale,
                                          std::vector<float>& transformed)
{
    transformed.resize (frequencies.size());
    if (scale == spectrumviewer::FrequencyScale::linear)
        std::copy (frequencies.begin(), frequencies.end(), transformed.begin());
    else
        std::transform (frequencies.begin(), frequencies.end(), transformed.begin(), [] (float frequency)
                        { return std::log10 (frequency); });
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

SpectrumCanvas::SpectrumCanvas (SpectrumViewer* n, SpectrumDisplaySettings& settings)
    : Visualizer ((GenericProcessor*) n),
      processor (n),
      displaySettings (settings)
{
    refreshRate = 60;

    canvasPlot = std::make_unique<CanvasPlot> (processor);

    viewport = std::make_unique<Viewport>();
    viewport->setViewedComponent (canvasPlot.get(), false);
    viewport->setScrollBarsShown (true, true);
    viewport->setScrollBarThickness (12);
    addAndMakeVisible (viewport.get());

    createControls();
    applyDisplaySettings();

    statusTimer.startTimerHz (4);
}

SpectrumCanvas::~SpectrumCanvas()
{
    statusTimer.stopTimer();
}

namespace
{
/** One fixed-size item in a row of controls.

    flexShrink defaults to 1, which would squeeze controls to unreadable widths
    instead of letting the bar scroll, so it is pinned here. */
FlexItem rowItem (Component& component, int width, int height, int gapBefore)
{
    auto item = FlexItem (component)
                    .withWidth (static_cast<float> (width))
                    .withHeight (static_cast<float> (height));
    item.flexShrink = 0.0f;

    // Set the field rather than withMargin (FlexItem::Margin {...}): the
    // four-argument Margin constructor is not among the JUCE symbols the host
    // exports, so calling it fails to link in a plugin.
    item.margin.left = static_cast<float> (gapBefore);
    return item;
}
} // namespace

SpectrumCanvas::ControlGroup::ControlGroup (const String& titleText)
    : GroupComponent (titleText + "Group", titleText)
{
}

void SpectrumCanvas::ControlGroup::addPair (int row,
                                            Label* label,
                                            Component* control,
                                            int labelWidth,
                                            int controlWidth)
{
    jassert (isPositiveAndBelow (row, rowCount));
    entries.push_back ({ row, label, control, labelWidth, controlWidth });
    if (label != nullptr)
        addAndMakeVisible (label);
    if (control != nullptr)
        addAndMakeVisible (control);
}

std::array<int, SpectrumCanvas::ControlGroup::rowCount>
    SpectrumCanvas::ControlGroup::getRowWidths() const
{
    std::array<int, rowCount> widths {};
    for (const auto& entry : entries)
    {
        if (! entry.isShowing())
            continue;

        auto& width = widths[static_cast<std::size_t> (entry.row)];
        if (width > 0)
            width += controlSpacing;
        width += entry.width();
    }
    return widths;
}

int SpectrumCanvas::ControlGroup::getPreferredWidth() const
{
    const auto widths = getRowWidths();
    const auto widest = *std::max_element (widths.begin(), widths.end());
    if (widest == 0)
        return 0;

    // Never narrower than the title the outline draws inline, or the title is
    // clipped and the group reads as belonging to its neighbour.
    const auto titleWidth = GlyphArrangement::getStringWidthInt (
                                FontOptions { static_cast<float> (groupTitleHeight) + 1.0f },
                                getText())
                            + 2 * groupPadding;
    return std::max (widest, titleWidth) + 2 * groupPadding;
}

void SpectrumCanvas::ControlGroup::lookAndFeelChanged()
{
    setColour (GroupComponent::outlineColourId,
               findColour (ThemeColours::defaultText).withAlpha (0.35f));
    setColour (GroupComponent::textColourId,
               findColour (ThemeColours::defaultText));
}

void SpectrumCanvas::ControlGroup::resized()
{
    auto area = getLocalBounds().reduced (groupPadding, 0);
    area.removeFromTop (groupTitleHeight);

    // Label visibility follows its control's, which updateAmplitudeRangeControls()
    // owns for the only pair that hides.
    for (auto row = 0; row < rowCount; ++row)
    {
        if (row > 0)
            area.removeFromTop (groupRowGap);
        const auto rowArea = area.removeFromTop (controlHeight);

        FlexBox flex;
        flex.alignItems = FlexBox::AlignItems::center;
        for (const auto& entry : entries)
        {
            if (entry.row != row || ! entry.isShowing())
                continue;

            const auto gap = flex.items.isEmpty() ? 0 : controlSpacing;
            if (entry.label != nullptr)
                flex.items.add (rowItem (*entry.label, entry.labelWidth, controlHeight, gap));
            flex.items.add (rowItem (*entry.control,
                                     entry.controlWidth,
                                     controlHeight,
                                     entry.label != nullptr ? 0 : gap));
        }

        if (! flex.items.isEmpty())
            flex.performLayout (rowArea.toFloat());
    }
}

void SpectrumCanvas::createControls()
{
    auto makeLabel = [this] (const char* componentName, const char* text)
    {
        auto label = std::make_unique<Label> (componentName, text);
        label->setFont (FontOptions ("Inter", "Regular", 14.0f));
        return label;
    };

    auto makeComboBox = [this] (const char* componentName,
                                std::initializer_list<const char*> items,
                                const char* tooltip = nullptr)
    {
        auto box = std::make_unique<ComboBox> (componentName);
        StringArray itemList;
        for (const auto* item : items)
            itemList.add (item);
        box->addItemList (itemList, 1);
        box->addListener (this);
        if (tooltip != nullptr)
            box->setTooltip (tooltip);
        return box;
    };

    optionsContent = std::make_unique<Component> ("Spectrum options");
    mainOptionsBar = std::make_unique<Component> ("Spectrum display controls");
    optionsContent->addAndMakeVisible (mainOptionsBar.get());
    optionsDrawer = std::make_unique<Component> ("Spectrum advanced controls");
    optionsContent->addChildComponent (optionsDrawer.get());

    optionsViewport = std::make_unique<Viewport> ("Spectrum options viewport");
    optionsViewport->setViewedComponent (optionsContent.get(), false);
    optionsViewport->setScrollBarsShown (false, true);
    optionsViewport->setScrollBarThickness (scrollBarThickness);
    addAndMakeVisible (optionsViewport.get());

    displayLabel = makeLabel ("DisplayTypeLabel", "Display");
    displayType = makeComboBox (
        "Display Type", { "Power Spectrum", "Spectrogram" },
        "The spectrogram shows the first selected channel only. Background removal "
        "applies to it; the background fit, peak envelope and reference comparison "
        "are drawn on the power spectrum only.");

    frequencyLabel = makeLabel ("FreqRangeLabel", "Frequency range");
    frequencyRange = makeComboBox ("FreqRange", { "0 - 100", "0 - 500", "0 - 1000", "Full" });
    freqRanges.add (Range (0, 100));
    freqRanges.add (Range (0, 500));
    freqRanges.add (Range (0, 1000));
    freqRanges.add (Range (0, 1000)); // Updated to the selected stream's Nyquist.

    profileLabel = makeLabel ("AnalysisProfileLabel", "Analysis");
    analysisProfile = makeComboBox ("AnalysisProfile", { "Fast", "Balanced", "Fine" });

    scaleLabel = makeLabel ("FrequencyScaleLabel", "Frequency scale");
    frequencyScale = makeComboBox ("FrequencyScale", { "Linear", "Log" });

    amplitudeLabel = makeLabel ("AmplitudeDisplayLabel", "Y units");
    amplitudeDisplay = makeComboBox ("AmplitudeDisplay", { "PSD", "ASD" });

    for (auto* control : { (Component*) displayLabel.get(), (Component*) displayType.get(),
                           (Component*) profileLabel.get(), (Component*) analysisProfile.get(),
                           (Component*) frequencyLabel.get(), (Component*) frequencyRange.get(),
                           (Component*) scaleLabel.get(), (Component*) frequencyScale.get(),
                           (Component*) amplitudeLabel.get(), (Component*) amplitudeDisplay.get() })
        mainOptionsBar->addAndMakeVisible (control);

    amplitudeRangeLabel = makeLabel ("AmplitudeRangeLabel", "dB Range");
    amplitudeRangeMode = makeComboBox ("AmplitudeRangeMode", { "Auto", "Fixed" });

    minimumDbLabel = makeLabel ("MinimumDbLabel", "Min dB");
    minimumDb = std::make_unique<Slider> ("MinimumDb");
    minimumDb->setRange (-240.0, 100.0, 1.0);
    minimumDb->setSliderStyle (Slider::LinearHorizontal);
    minimumDb->setTextBoxStyle (Slider::TextBoxLeft, false, 55, controlHeight);
    minimumDb->addListener (this);

    maximumDbLabel = makeLabel ("MaximumDbLabel", "Max dB");
    maximumDb = std::make_unique<Slider> ("MaximumDb");
    maximumDb->setRange (-220.0, 120.0, 1.0);
    maximumDb->setSliderStyle (Slider::LinearHorizontal);
    maximumDb->setTextBoxStyle (Slider::TextBoxLeft, false, 55, controlHeight);
    maximumDb->addListener (this);

    automaticRangeLabel = makeLabel ("AutomaticRangeLabel", "Awaiting spectrum...");
    automaticRangeLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));

    captureDurationLabel = makeLabel ("CaptureDurationLabel", "Capture Length");
    captureDuration = makeComboBox (
        "CaptureDuration", { "10 s", "30 s", "60 s" },
        "Averages non-overlapping two-second Fine spectra in linear power");

    captureAction = std::make_unique<UtilityButton> ("Capture");
    captureAction->addListener (this);

    captureStatusLabel = makeLabel ("CaptureStatus", "Live");
    captureStatusLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));

    comparisonModeLabel = makeLabel ("SpectrumComparisonModeLabel", "Comparison");
    comparisonMode = makeComboBox ("SpectrumComparisonMode",
                                   { "Absolute", "Overlay", "Delta" });

    baselineLabel = makeLabel ("AperiodicDisplayLabel", "Background");
    baselineDisplay = makeComboBox (
        "AperiodicDisplay", { "Off", "Show fit", "Remove" },
        "Show or subtract a robust broad spectral background; the PSD estimate is unchanged");

    peakEnvelope = std::make_unique<ToggleButton> ("Peak envelope");
    peakEnvelope->setTooltip (
        "Show the maximum spectral power contributing to each display column");
    peakEnvelope->addListener (this);

    setReferenceAction = std::make_unique<UtilityButton> ("Set Ref");
    setReferenceAction->setTooltip ("Make the frozen capture the comparison reference");
    setReferenceAction->addListener (this);
    clearReferenceAction = std::make_unique<UtilityButton> ("Clear Ref");
    clearReferenceAction->addListener (this);
    importReferenceAction = std::make_unique<UtilityButton> ("Import...");
    importReferenceAction->setTooltip (
        "Load a baseline saved with Export, for example one captured under ideal "
        "conditions, and compare every selected channel with it");
    importReferenceAction->addListener (this);
    exportReferenceAction = std::make_unique<UtilityButton> ("Export...");
    exportReferenceAction->setTooltip (
        "Save one channel of the reference as a baseline, to compare against in "
        "later sessions");
    exportReferenceAction->addListener (this);

    referenceStatusLabel = makeLabel ("SpectrumReferenceStatus", "No reference");
    referenceStatusLabel->setMouseCursor (MouseCursor::PointingHandCursor);
    referenceStatusLabel->setFont (FontOptions ("Inter", "Regular", 13.0f));

    createControlGroups();

    showHideOptionsButton = std::make_unique<ShowHideSpectrumOptionsButton>();
    showHideOptionsButton->onClick = [this]
    {
        setOptionsDrawerOpen (showHideOptionsButton->getToggleState());
    };
    addAndMakeVisible (showHideOptionsButton.get());
}

void SpectrumCanvas::createControlGroups()
{
    // The drawer holds four unrelated jobs. Grouping them by job keeps the
    // comparison mode next to the reference buttons it applies to, which the
    // previous flat row separated by five unrelated controls, and lets the
    // drawer wrap a whole job onto the next line instead of a stray slider.
    const auto addGroup = [this] (const char* captionText)
    {
        controlGroups.push_back (std::make_unique<ControlGroup> (captionText));
        auto* group = controlGroups.back().get();
        optionsDrawer->addAndMakeVisible (group);
        return group;
    };

    // Two rows per group, assigned here rather than computed: it keeps each
    // group about half as wide as a single row would, so far more of the
    // drawer fits before it has to scroll.
    // The two dB sliders sit side by side on the second row, so they read as a
    // pair and line up with the automatic readout they replace.
    auto* amplitude = addGroup ("AMPLITUDE");
    amplitude->addPair (0, amplitudeRangeLabel.get(), amplitudeRangeMode.get(), 72, 90);
    amplitude->addPair (1, maximumDbLabel.get(), maximumDb.get(), 48, 110);
    amplitude->addPair (1, minimumDbLabel.get(), minimumDb.get(), 48, 110);
    amplitude->addPair (1, nullptr, automaticRangeLabel.get(), 0, 150);

    auto* traces = addGroup ("TRACES");
    traces->addPair (0, baselineLabel.get(), baselineDisplay.get(), 82, 100);
    traces->addPair (1, nullptr, peakEnvelope.get(), 0, 125);

    // Set Ref belongs with the capture it acts on: it only ever applies to a
    // frozen capture, while the reference group also serves imported baselines.
    auto* capture = addGroup ("CAPTURE");
    capture->addPair (0, captureDurationLabel.get(), captureDuration.get(), 100, 90);
    capture->addPair (0, nullptr, captureStatusLabel.get(), 0, 120);
    capture->addPair (1, nullptr, captureAction.get(), 0, 95);
    capture->addPair (1, nullptr, setReferenceAction.get(), 0, 85);

    auto* reference = addGroup ("REFERENCE");
    reference->addPair (0, comparisonModeLabel.get(), comparisonMode.get(), 82, 100);
    reference->addPair (0, nullptr, clearReferenceAction.get(), 0, 90);
    reference->addPair (1, nullptr, importReferenceAction.get(), 0, 85);
    reference->addPair (1, nullptr, exportReferenceAction.get(), 0, 85);
    reference->addPair (1, nullptr, referenceStatusLabel.get(), 0, 205);
}

void SpectrumCanvas::applyDisplaySettings()
{
    // Guards the listener callbacks: every setSelectedId below notifies, and
    // without this each one would write the value it just read back into
    // displaySettings and re-issue processor calls.
    const ScopedValueSetter<bool> guard (applyingSettings, true);

    displaySettings.sanitize();

    displayType->setSelectedId (displaySettings.displayTypeId, dontSendNotification);
    frequencyRange->setSelectedId (displaySettings.frequencyRangeId, dontSendNotification);
    analysisProfile->setSelectedId (displaySettings.analysisProfileId, dontSendNotification);
    frequencyScale->setSelectedId (displaySettings.frequencyScaleId, dontSendNotification);
    amplitudeDisplay->setSelectedId (displaySettings.amplitudeDisplayId, dontSendNotification);
    baselineDisplay->setSelectedId (displaySettings.aperiodicDisplayId, dontSendNotification);
    amplitudeRangeMode->setSelectedId (displaySettings.amplitudeRangeModeId,
                                       dontSendNotification);
    captureDuration->setSelectedId (displaySettings.captureDurationId, dontSendNotification);
    comparisonMode->setSelectedId (displaySettings.comparisonModeId, dontSendNotification);
    peakEnvelope->setToggleState (displaySettings.peakEnvelopeVisible, dontSendNotification);
    minimumDb->setValue (displaySettings.minimumDb, dontSendNotification);
    maximumDb->setValue (displaySettings.maximumDb, dontSendNotification);

    refreshNyquistRangeItem();

    // Push everything through to the processor and the plot.
    processor->setAnalysisProfile (
        static_cast<SpectrumAnalysisProfile> (displaySettings.analysisProfileId));
    processor->setFrequencyScale (displaySettings.frequencyScaleId == 2
                                      ? spectrumviewer::FrequencyScale::logarithmic
                                      : spectrumviewer::FrequencyScale::linear);
    processor->setAperiodicDisplayMode (
        static_cast<spectrumviewer::AperiodicDisplayMode> (displaySettings.aperiodicDisplayId));
    processor->setSpectrumComparisonMode (
        static_cast<spectrumviewer::SpectrumComparisonMode> (displaySettings.comparisonModeId));
    applyFrequencyRange();

    auto* plot = canvasPlot.get();
    plot->setAmplitudeDisplay (
        static_cast<SpectrumAmplitudeDisplay> (displaySettings.amplitudeDisplayId));
    plot->setAperiodicDisplayMode (
        static_cast<spectrumviewer::AperiodicDisplayMode> (displaySettings.aperiodicDisplayId));
    plot->setPeakEnvelopeVisible (displaySettings.peakEnvelopeVisible);
    applyAmplitudeRangeToPlot();

    setDisplayType (static_cast<DisplayType> (displaySettings.displayTypeId));

    optionsDrawer->setVisible (displaySettings.optionsDrawerOpen);
    showHideOptionsButton->setToggleState (displaySettings.optionsDrawerOpen,
                                           dontSendNotification);

    updateAmplitudeRangeControls();
    resized();
}

void SpectrumCanvas::refreshNyquistRangeItem()
{
    auto* stream = processor->getDataStream (processor->getActiveStreamId());
    if (stream == nullptr)
        return;

    const auto maximumFrequency = stream->getSampleRate() / 2.0f;
    freqRanges.set (3, Range (0, static_cast<int> (maximumFrequency)));
    frequencyRange->changeItemText (
        4, "Full (0 - " + String (maximumFrequency, 0) + ")");

    // changeItemText does not refresh the closed box, so reselect to redraw.
    if (frequencyRange->getSelectedId() == 4)
        frequencyRange->setText (frequencyRange->getItemText (3), dontSendNotification);
}

void SpectrumCanvas::applyFrequencyRange()
{
    const auto selectedIndex = frequencyRange->getSelectedItemIndex();
    if (! isPositiveAndBelow (selectedIndex, freqRanges.size()))
        return;

    const auto range = freqRanges[selectedIndex];
    if (frequencyRange->getSelectedId() == 4)
        processor->setFullFrequencyRange();
    else
        processor->setFrequencyRange (range);

    canvasPlot->setFrequencyRange (range.getStart(), range.getEnd(),
                                   processor->getFreqStep());
}

void SpectrumCanvas::resized()
{
    // The bars wrap, so how much height they need depends on the width. Lay
    // them out first and give the viewport whatever is left.
    const auto controlsHeight = layOutControls();
    const auto width = getWidth();
    const auto height = getHeight();

    int plotWidth, plotHeight;
    if (currentDisplayType == POWER_SPECTRUM)
    {
        // Below this width the plot scrolls rather than shrinking into
        // unreadability; the viewport already provides the scrollbars.
        constexpr int minimumPlotWidth = 800;
        constexpr int minimumPlotHeight = 600;
        constexpr int plotMargin = 40;
        constexpr int plotVerticalMargin = 50;

        const auto availableWidth = width - canvasPlot->legendWidth - plotMargin;
        plotWidth = std::max (minimumPlotWidth, availableWidth);

        const auto availableHeight = height - controlsHeight - plotVerticalMargin;
        plotHeight = std::max (minimumPlotHeight, availableHeight);

        canvasPlot->setBounds (0, 0,
                               plotWidth + canvasPlot->legendWidth + plotMargin,
                               plotHeight + plotVerticalMargin);
    }
    else
    {
        canvasPlot->setBounds (0, 0, width, height - controlsHeight);
    }

    viewport->setBounds (0, 0, width, std::max (0, height - controlsHeight));
}

int SpectrumCanvas::layOutControls()
{
    const auto drawerOpen = displaySettings.optionsDrawerOpen;
    const auto contentHeight = optionsBarHeight + (drawerOpen ? optionsDrawerHeight : 0);

    FlexBox mainRow;
    mainRow.alignItems = FlexBox::AlignItems::flexStart;
    auto mainRowWidth = 0;
    const auto addMainPair = [&] (Label* label, Component* control,
                                  int labelWidth, int controlWidth)
    {
        const auto gap = mainRow.items.isEmpty() ? 0 : controlSpacing;
        mainRow.items.add (rowItem (*label, labelWidth, controlHeight, gap));
        mainRow.items.add (rowItem (*control, controlWidth, controlHeight, 0));
        mainRowWidth += gap + labelWidth + controlWidth;
    };
    addMainPair (displayLabel.get(), displayType.get(), 56, 120);
    addMainPair (profileLabel.get(), analysisProfile.get(), 56, 100);
    addMainPair (frequencyLabel.get(), frequencyRange.get(), 106, 130);
    addMainPair (scaleLabel.get(), frequencyScale.get(), 100, 90);
    addMainPair (amplitudeLabel.get(), amplitudeDisplay.get(), 50, 80);

    FlexBox drawerRow;
    drawerRow.alignItems = FlexBox::AlignItems::flexStart;
    auto drawerRowWidth = 0;
    for (auto& group : controlGroups)
    {
        const auto width = group->getPreferredWidth();
        group->setVisible (width > 0);
        if (width == 0)
            continue;

        const auto gap = drawerRow.items.isEmpty() ? 0 : groupSpacing;
        drawerRow.items.add (rowItem (*group, width, groupHeight, gap));
        drawerRowWidth += gap + width;
    }

    // The Options button sits beside the scrolling area rather than over it,
    // so no control can ever scroll underneath it.
    const auto buttonStrip = optionsButtonWidth + 2 * barPadding;
    const auto viewportWidth = std::max (1, getWidth() - buttonStrip);

    // Below this the bar scrolls rather than compressing controls to
    // unreadable widths. The drawer only counts while it is open.
    const auto requiredWidth = std::max (mainRowWidth, drawerOpen ? drawerRowWidth : 0)
                               + 2 * barPadding;
    const auto scrolls = requiredWidth > viewportWidth;
    const auto totalHeight = contentHeight + (scrolls ? scrollBarThickness : 0);

    const auto top = getHeight() - totalHeight;
    optionsViewport->setBounds (0, top, viewportWidth, totalHeight);

    // Beside the main bar's row, not the top of the options area: the button
    // belongs to that row and should not move when the drawer opens.
    showHideOptionsButton->setBounds (viewportWidth + barPadding,
                                      getHeight() - controlHeight - barPadding,
                                      optionsButtonWidth,
                                      controlHeight);

    optionsContent->setBounds (0, 0, std::max (viewportWidth, requiredWidth), contentHeight);

    auto area = optionsContent->getLocalBounds();
    optionsDrawer->setVisible (drawerOpen);
    if (drawerOpen)
    {
        optionsDrawer->setBounds (area.removeFromTop (optionsDrawerHeight));
        drawerRow.performLayout (optionsDrawer->getLocalBounds().reduced (barPadding).toFloat());
    }

    mainOptionsBar->setBounds (area);
    mainRow.performLayout (mainOptionsBar->getLocalBounds().reduced (barPadding).toFloat());

    return totalHeight;
}

void SpectrumCanvas::refreshState() {}

void SpectrumCanvas::paint (Graphics& g)
{
    // The bars scroll inside optionsViewport, so paint the strip the viewport
    // and the pinned Options button occupy rather than the bars' own bounds,
    // which are in the scrolling content's coordinates.
    const auto top = optionsViewport->getY();
    g.setColour (findColour (ThemeColours::componentBackground));
    g.fillRect (0, top, getWidth(), getHeight() - top);

    g.setColour (findColour (ThemeColours::defaultText).withAlpha (0.4f));
    g.drawHorizontalLine (top, 0.0f, static_cast<float> (getWidth()));

    g.drawVerticalLine (optionsViewport->getRight() + 1, top, getHeight());
}

void SpectrumCanvas::setOptionsDrawerOpen (bool shouldBeOpen)
{
    displaySettings.optionsDrawerOpen = shouldBeOpen;
    optionsDrawer->setVisible (shouldBeOpen);
    showHideOptionsButton->setToggleState (shouldBeOpen, dontSendNotification);
    resized();
    repaint();
}

void SpectrumCanvas::updateSettings()
{
    canvasPlot->updateActiveChans();
    refreshNyquistRangeItem();

    // The selected stream may have a different Nyquist frequency, so the full
    // range has to be recomputed even though the selection has not changed.
    if (frequencyRange->getSelectedId() == 4)
        applyFrequencyRange();
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
                                 || readiness == SpectrumAnalysisReadiness::configurationFailed
                                 || readiness == SpectrumAnalysisReadiness::invalidSelection);
    if (unavailable)
    {
        if (! unavailableStateCleared)
        {
            // The legend is refreshed from frames while acquisition runs, so
            // clearing the traces alone would leave it naming the channels of
            // the route that just went away. Re-read the selection instead.
            canvasPlot->updateActiveChans();
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
        if (currentDisplayType == POWER_SPECTRUM)
            canvasPlot->plotPowerSpectrum (true);
        else
            canvasPlot->drawSpectrogram();
    }
}

void SpectrumCanvas::setDisplayType (DisplayType type)
{
    // Pause the refresh while the plot swaps its display type, so refresh()
    // cannot run against a half-changed plot. What matters is whether this
    // canvas's own animation timer is running, not whether the host is
    // acquiring: applyDisplaySettings() calls this from the constructor, where
    // CoreServices::getAcquisitionStatus() would reach a ControlPanel that
    // does not exist outside the full application.
    const auto wasAnimating = isTimerRunning();
    if (wasAnimating)
        stopCallbacks();

    currentDisplayType = type;
    canvasPlot->setDisplayType (type);

    if (wasAnimating)
        startCallbacks();

    resized();
}

void SpectrumCanvas::comboBoxChanged (ComboBox* cb)
{
    if (applyingSettings)
        return;

    if (cb == displayType.get())
    {
        displaySettings.displayTypeId = cb->getSelectedId();
        setDisplayType (static_cast<DisplayType> (displaySettings.displayTypeId));
    }
    else if (cb == frequencyRange.get())
    {
        displaySettings.frequencyRangeId = cb->getSelectedId();
        applyFrequencyRange();
    }
    else if (cb == analysisProfile.get())
    {
        displaySettings.analysisProfileId = cb->getSelectedId();
        processor->setAnalysisProfile (
            static_cast<SpectrumAnalysisProfile> (displaySettings.analysisProfileId));
    }
    else if (cb == frequencyScale.get())
    {
        displaySettings.frequencyScaleId = cb->getSelectedId();
        processor->setFrequencyScale (displaySettings.frequencyScaleId == 2
                                          ? spectrumviewer::FrequencyScale::logarithmic
                                          : spectrumviewer::FrequencyScale::linear);
    }
    else if (cb == amplitudeDisplay.get())
    {
        displaySettings.amplitudeDisplayId = cb->getSelectedId();
        canvasPlot->setAmplitudeDisplay (
            static_cast<SpectrumAmplitudeDisplay> (displaySettings.amplitudeDisplayId));
    }
    else if (cb == baselineDisplay.get())
    {
        displaySettings.aperiodicDisplayId = cb->getSelectedId();
        const auto mode = static_cast<spectrumviewer::AperiodicDisplayMode> (
            displaySettings.aperiodicDisplayId);
        processor->setAperiodicDisplayMode (mode);
        canvasPlot->setAperiodicDisplayMode (mode);
    }
    else if (cb == amplitudeRangeMode.get())
    {
        displaySettings.amplitudeRangeModeId = cb->getSelectedId();
        updateAmplitudeRangeControls();
        applyAmplitudeRangeToPlot();
        resized();
    }
    else if (cb == captureDuration.get())
    {
        displaySettings.captureDurationId = cb->getSelectedId();
    }
    else if (cb == comparisonMode.get())
    {
        displaySettings.comparisonModeId = cb->getSelectedId();
        processor->setSpectrumComparisonMode (
            static_cast<spectrumviewer::SpectrumComparisonMode> (
                displaySettings.comparisonModeId));
        updateAmplitudeRangeControls();
        resized();
    }
}

void SpectrumCanvas::sliderValueChanged (Slider* slider)
{
    if (applyingSettings)
        return;

    constexpr auto minimumSpanDb = SpectrumDisplaySettings::minimumSpanDb;
    if (slider == minimumDb.get()
        && minimumDb->getValue() > maximumDb->getValue() - minimumSpanDb)
        minimumDb->setValue (maximumDb->getValue() - minimumSpanDb, dontSendNotification);
    else if (slider == maximumDb.get()
             && maximumDb->getValue() < minimumDb->getValue() + minimumSpanDb)
        maximumDb->setValue (minimumDb->getValue() + minimumSpanDb, dontSendNotification);

    displaySettings.minimumDb = minimumDb->getValue();
    displaySettings.maximumDb = maximumDb->getValue();
    applyAmplitudeRangeToPlot();
}

void SpectrumCanvas::buttonClicked (Button* button)
{
    if (applyingSettings)
        return;

    if (button == peakEnvelope.get())
    {
        displaySettings.peakEnvelopeVisible = peakEnvelope->getToggleState();
        canvasPlot->setPeakEnvelopeVisible (displaySettings.peakEnvelopeVisible);
        return;
    }
    if (button == setReferenceAction.get())
    {
        // A fresh attempt. updateStatus() latches this again if the worker
        // drops the request too.
        referenceRequestWasDropped = false;
        if (processor->setCurrentCaptureAsReference())
            useReferenceForComparison();
        return;
    }
    if (button == clearReferenceAction.get())
    {
        referenceRequestWasDropped = false;
        processor->clearSpectrumReference();
        updateStatus();
        return;
    }
    if (button == importReferenceAction.get())
    {
        chooseReferenceFileToImport();
        return;
    }
    if (button == exportReferenceAction.get())
    {
        chooseChannelToExport();
        return;
    }
    if (button != captureAction.get())
        return;

    switch (processor->getCaptureState())
    {
        case SpectrumCaptureState::live:
        case SpectrumCaptureState::failed:
        {
            constexpr double durations[] { 10.0, 30.0, 60.0 };
            const auto index = jlimit (0, 2, captureDuration->getSelectedItemIndex());
            processor->startSpectrumCapture (durations[index]);
            break;
        }
        case SpectrumCaptureState::preparing:
        case SpectrumCaptureState::capturing:
            processor->cancelSpectrumCapture();
            break;
        case SpectrumCaptureState::frozen:
            processor->returnToLive();
            break;
        case SpectrumCaptureState::restoringLive:
            break;
    }
}

void SpectrumCanvas::updateAmplitudeRangeControls()
{
    const auto compatibleReference =
        processor->hasSpectrumReference()
        && processor->getReferenceCompatibility()
               == spectrumviewer::SpectrumReferenceCompatibility::compatible;
    const auto comparisonActive =
        comparisonMode->getSelectedId()
            != static_cast<int> (spectrumviewer::SpectrumComparisonMode::absolute)
        && compatibleReference;
    const auto delta =
        comparisonMode->getSelectedId()
            == static_cast<int> (spectrumviewer::SpectrumComparisonMode::deltaDb)
        && compatibleReference;
    const auto fixed = amplitudeRangeMode->getSelectedId() == 2 && ! delta;

    amplitudeRangeMode->setEnabled (! delta);
    minimumDb->setEnabled (fixed);
    maximumDb->setEnabled (fixed);
    minimumDb->setVisible (fixed);
    maximumDb->setVisible (fixed);
    minimumDbLabel->setVisible (fixed);
    maximumDbLabel->setVisible (fixed);
    automaticRangeLabel->setVisible (! fixed);
    baselineDisplay->setEnabled (! comparisonActive);
    baselineDisplay->setTooltip (
        comparisonActive
            ? "Aperiodic display is unavailable while comparing with a reference"
            : "Show or subtract a robust broad spectral background; the PSD estimate is unchanged");
}

void SpectrumCanvas::applyAmplitudeRangeToPlot()
{
    canvasPlot->setFixedAmplitudeRange (static_cast<float> (displaySettings.minimumDb),
                                        static_cast<float> (displaySettings.maximumDb));
    canvasPlot->setAmplitudeRangeMode (
        static_cast<spectrumviewer::AmplitudeRangeMode> (
            displaySettings.amplitudeRangeModeId));
}

void SpectrumCanvas::updateStatus()
{
    const auto capture = processor->getCaptureState();
    const auto readiness = processor->getAnalysisReadiness();
    // Nothing can be captured from a selection that cannot be routed, and
    // nothing is running to capture from when acquisition is stopped.
    const auto analysisAvailable =
        readiness != SpectrumAnalysisReadiness::stopped
        && readiness != SpectrumAnalysisReadiness::invalidSelection;
    captureStatusLabel->setTooltip ({});
    captureDuration->setEnabled (capture == SpectrumCaptureState::live
                                 || capture == SpectrumCaptureState::failed);
    analysisProfile->setEnabled (capture == SpectrumCaptureState::live
                                 || capture == SpectrumCaptureState::failed);
    captureAction->setEnabled (analysisAvailable
                               && capture != SpectrumCaptureState::restoringLive);

    switch (capture)
    {
        case SpectrumCaptureState::preparing:
            captureAction->setLabel ("Cancel");
            captureAction->setTooltip ("Cancel this capture and return to the live spectrum");
            captureStatusLabel->setText ("Preparing Fine...", dontSendNotification);
            captureStatusLabel->setTooltip (
                "Preparing 2 s, NW=3, K=4 non-overlapping Fine analysis");
            break;
        case SpectrumCaptureState::capturing:
        {
            captureAction->setLabel ("Cancel");
            captureAction->setTooltip ("Cancel this capture and return to the live spectrum");
            const auto lostWindows = processor->getCaptureFailedWindowCount()
                                     + processor->getCaptureShedWindowCount()
                                     + processor->getCaptureDiscontinuityCount();
            // Analyzed against elapsed, live: a capture losing windows to input
            // gaps otherwise looks exactly like one that is simply slow.
            captureStatusLabel->setText (
                "Fine " + String (processor->getCaptureIncludedWindowCount()) + "/"
                    + String (processor->getCaptureTargetWindowCount()) + " ("
                    + String (processor->getCaptureAnalyzedSeconds(), 0) + "/"
                    + String (processor->getCaptureWallSpanSeconds(), 0) + " s)"
                    + (lostWindows > 0 ? " !" : ""),
                dontSendNotification);
            captureStatusLabel->setTooltip (
                "2 s, NW=3, K=4 non-overlapping Fine spectra; analyzed / elapsed "
                "seconds; failed "
                + String (processor->getCaptureFailedWindowCount()) + ", shed "
                + String (processor->getCaptureShedWindowCount())
                + ", discontinuities "
                + String (processor->getCaptureDiscontinuityCount()));
            break;
        }
        case SpectrumCaptureState::frozen:
        {
            captureAction->setLabel ("Go Live");
            // Going live discards the frozen result unless it was kept as the
            // reference, which is the one thing worth knowing before the click.
            const auto isReference = processor->hasSpectrumReference()
                                     && processor->getReferenceCaptureId()
                                            == processor->getCaptureId();
            if (isReference)
                captureAction->setTooltip (
                    "Resume the live spectrum. The frozen result stays as the reference.");
            else
                captureAction->setTooltip (
                    String ("Discard the frozen result and resume the live spectrum.")
                    + (processor->hasRetainedCapture()
                           ? " Press Set Ref first to keep it as the reference."
                           : ""));
            const auto warning = processor->getCaptureFailedWindowCount()
                                     + processor->getCaptureShedWindowCount()
                                     + processor->getCaptureDiscontinuityCount()
                                 > 0;
            const auto span = String (processor->getCaptureAnalyzedSeconds(), 0) + "/"
                              + String (processor->getCaptureWallSpanSeconds(), 0) + " s";
            // Once the result is the reference, what is left to do with this
            // capture is go live, so the status says that instead of the span.
            captureStatusLabel->setText (
                (isReference ? "Ref set; Go Live" : "Frozen " + span) + (warning ? " !" : ""),
                dontSendNotification);
            captureStatusLabel->setTooltip (
                (isReference ? "Frozen " + span + ", set as the reference.\n" : String())
                + "2 s, NW=3, K=4; analyzed / wall-span seconds; failed "
                + String (processor->getCaptureFailedWindowCount()) + ", shed "
                + String (processor->getCaptureShedWindowCount())
                + ", discontinuities "
                + String (processor->getCaptureDiscontinuityCount()));
            break;
        }
        case SpectrumCaptureState::restoringLive:
            captureAction->setLabel ("Restoring...");
            captureAction->setTooltip ("Preparing the live analysis");
            captureStatusLabel->setText ("Frozen", dontSendNotification);
            break;
        case SpectrumCaptureState::failed:
            captureAction->setLabel ("Retry");
            captureAction->setTooltip ("Retry the spectrum capture");
            captureStatusLabel->setText ("Capture failed", dontSendNotification);
            captureStatusLabel->setTooltip (
                "The capture could not be prepared or could not be retained. "
                "Retry, or select fewer channels if memory is short.");
            break;
        case SpectrumCaptureState::live:
        default:
            captureAction->setLabel ("Capture");
            captureAction->setTooltip ("Average Fine spectra, then freeze the result");
            captureStatusLabel->setText ("Live", dontSendNotification);
            break;
    }

    const auto droppedReferenceRequests = processor->getDroppedReferenceRequestCount();
    if (droppedReferenceRequests != seenDroppedReferenceRequests)
    {
        seenDroppedReferenceRequests = droppedReferenceRequests;
        referenceRequestWasDropped = true;

        // Setting a reference optimistically switches to Fine and Overlay on
        // the click. There is no reference to overlay, so put the comparison
        // back rather than leaving a mode selected that does nothing.
        comparisonMode->setSelectedId (
            static_cast<int> (spectrumviewer::SpectrumComparisonMode::absolute),
            sendNotification);
    }

    updateReferenceStatus (capture);

    const auto wasFixed = minimumDb->isVisible();
    updateAmplitudeRangeControls();
    if (wasFixed != minimumDb->isVisible())
        resized();

    if (amplitudeRangeMode->getSelectedId() == 1)
    {
        if (canvasPlot->hasAutomaticAmplitudeRange())
        {
            const auto range = canvasPlot->getAmplitudeRange();
            automaticRangeLabel->setText (String (range.minimum, 1) + " to "
                                              + String (range.maximum, 1) + " dB",
                                          dontSendNotification);
        }
        else
            automaticRangeLabel->setText ("Awaiting spectrum...", dontSendNotification);
    }
}

namespace
{
/** Channels as the channel selector numbers them, from one. */
String describeChannels (const std::vector<int>& indices)
{
    StringArray numbers;
    for (const auto index : indices)
        numbers.add (String (index + 1));
    return (indices.size() == 1 ? "channel " : "channels ") + numbers.joinIntoString (", ");
}

/** What the user has to change for the reference to apply, or empty. */
String describeMismatch (spectrumviewer::SpectrumReferenceMismatch mismatch,
                         const spectrumviewer::CapturedSpectrum& reference)
{
    using spectrumviewer::SpectrumReferenceMismatch;
    switch (mismatch)
    {
        case SpectrumReferenceMismatch::stream:
            return "It was captured on a different stream. Select that stream to compare.";
        case SpectrumReferenceMismatch::sampleRate:
            return "It was recorded at " + String (reference.getDescriptor().sampleRateHz, 2)
                   + " Hz. Select a stream with the same sample rate.";
        case SpectrumReferenceMismatch::estimator:
            return "Select Fine analysis to compare with it.";
        case SpectrumReferenceMismatch::channels:
            return "Select the same channels it was captured on: "
                   + describeChannels (reference.getSourceChannelIndices()) + ".";
        case SpectrumReferenceMismatch::units:
        {
            StringArray units;
            for (const auto& unit : reference.getSourceChannelUnits())
                units.add (unit);
            return "The selected channels' units differ from the reference's ("
                   + units.joinIntoString (", ") + ").";
        }
        case SpectrumReferenceMismatch::none:
        default:
            return {};
    }
}

/** A capture time, with the date when it is asked for or is not today's. An
    imported reference always asks, since it usually comes from another day. */
String describeCaptureTime (std::int64_t unixMilliseconds, bool withDate)
{
    const Time captured (unixMilliseconds);
    const auto now = Time::getCurrentTime();
    const auto today = captured.getYear() == now.getYear()
                       && captured.getDayOfYear() == now.getDayOfYear();
    return captured.formatted (today && ! withDate ? "%H:%M:%S" : "%Y-%m-%d %H:%M");
}
} // namespace

void SpectrumCanvas::updateReferenceStatus (SpectrumCaptureState capture)
{
    const auto reference = processor->getSpectrumReference();
    const auto fileError = processor->getReferenceFileError();

    // A baseline is already a one-channel file, so there is nothing to export.
    exportReferenceAction->setEnabled (reference != nullptr && ! reference->isBaseline());
    setReferenceAction->setEnabled (capture == SpectrumCaptureState::frozen
                                    && processor->hasRetainedCapture());
    // Clear also dismisses a failure message, so it stays available for one.
    clearReferenceAction->setEnabled (reference != nullptr || referenceRequestWasDropped
                                      || fileError.isNotEmpty());
    comparisonMode->setEnabled (reference != nullptr);

    if (fileError.isNotEmpty())
    {
        referenceStatusLabel->setText ("Reference file error", dontSendNotification);
        referenceStatusLabel->setTooltip (
            fileError
            + (reference != nullptr ? "\nThe current reference is unchanged." : String()));
        return;
    }

    if (reference == nullptr)
    {
        referenceStatusLabel->setText (
            referenceRequestWasDropped ? "Reference unavailable" : "No reference",
            dontSendNotification);
        referenceStatusLabel->setTooltip (
            referenceRequestWasDropped
                ? "The frozen capture could not be retained, so it could not "
                  "become the reference. Capture again."
                : String());
        return;
    }

    referenceRequestWasDropped = false;
    const auto compatibility = processor->getReferenceCompatibility();
    const auto incompatible =
        compatibility == spectrumviewer::SpectrumReferenceCompatibility::incompatible;

    // Whether the frozen capture is the reference is the capture status's to
    // say; this label always says which reference is held.
    const auto baseline = reference->isBaseline();
    referenceStatusLabel->setText (
        (baseline ? "Baseline " : "Session ref ")
            + describeCaptureTime (reference->getCapturedAtUnixMilliseconds(), baseline)
            + (incompatible ? " (incompatible)" : ""),
        dontSendNotification);

    StringArray tooltip;
    if (incompatible)
        tooltip.add ("Not comparable. "
                     + describeMismatch (processor->getReferenceMismatch(), *reference));
    else if (compatibility == spectrumviewer::SpectrumReferenceCompatibility::unchecked)
        tooltip.add ("Not yet compared with live analysis. It is checked once "
                     "acquisition runs with Fine analysis.");

    const auto capturedAt = Time (reference->getCapturedAtUnixMilliseconds())
                                .formatted ("%Y-%m-%d %H:%M:%S");
    if (baseline)
    {
        tooltip.add ("Baseline from a file. Every selected channel is compared with it.");

        // Where it was recorded, which no longer has to match anything.
        const auto& labels = reference->getSourceLabels();
        auto recorded = "Captured " + capturedAt + " on "
                        + describeChannels (reference->getSourceChannelIndices());
        if (! labels.channelNames.empty() && ! labels.channelNames.front().empty())
            recorded << " (\"" << String (labels.channelNames.front()) << "\")";
        if (! labels.streamName.empty())
            recorded << " of stream \"" << String (labels.streamName) << "\"";
        const auto& units = reference->getSourceChannelUnits().front();
        if (! units.empty())
            recorded << ", in " << String (units);
        tooltip.add (recorded + ".");
    }
    else
        tooltip.add ("Captured this session, " + capturedAt + ", on "
                     + describeChannels (reference->getSourceChannelIndices())
                     + ". Export saves one of its channels as a baseline.");

    const auto file = processor->getSpectrumReferenceFile();
    if (file != File())
        tooltip.add ("File: " + file.getFullPathName());
    tooltip.add ("Held until cleared, across acquisition runs.");
    referenceStatusLabel->setTooltip (tooltip.joinIntoString ("\n"));
}

void SpectrumCanvas::useReferenceForComparison()
{
    analysisProfile->setSelectedId (static_cast<int> (SpectrumAnalysisProfile::fine),
                                    sendNotification);
    comparisonMode->setSelectedId (
        static_cast<int> (spectrumviewer::SpectrumComparisonMode::overlay),
        sendNotification);
}

File SpectrumCanvas::getReferenceDirectory() const
{
    if (referenceDirectory.isDirectory())
        return referenceDirectory;
    // A baseline a saved session loaded, before either dialog has been used.
    const auto current = processor->getSpectrumReferenceFile();
    return current != File() ? current.getParentDirectory()
                             : File::getSpecialLocation (File::userDocumentsDirectory);
}

void SpectrumCanvas::chooseReferenceFileToImport()
{
    referenceFileChooser = std::make_unique<FileChooser> ("Import a baseline",
                                                          getReferenceDirectory(),
                                                          "*.json");
    referenceFileChooser->launchAsync (
        FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
        [this] (const FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == File())
                return;

            referenceDirectory = file.getParentDirectory();
            referenceRequestWasDropped = false;
            if (processor->importSpectrumReference (file).wasOk())
                useReferenceForComparison();
            updateStatus();
        });
}

void SpectrumCanvas::chooseChannelToExport()
{
    const auto reference = processor->getSpectrumReference();
    if (reference == nullptr || reference->isBaseline())
        return;
    if (reference->getChannelCount() == 1)
    {
        chooseReferenceFileToExport (0);
        return;
    }

    PopupMenu menu;
    menu.addSectionHeader ("Export which channel as a baseline?");
    const auto& indices = reference->getSourceChannelIndices();
    for (std::size_t channel = 0; channel < indices.size(); ++channel)
    {
        const auto number = "channel " + String (indices[channel] + 1);
        const auto name = processor->getChanName (reference->getSourceStreamId(), indices[channel]);
        menu.addItem (static_cast<int> (channel) + 1,
                      name.equalsIgnoreCase (number) ? name : name + " (" + number + ")");
    }

    // The menu outlives this call, and the reference may change while it is
    // open, so the choice is applied only to the reference it was made for.
    menu.showMenuAsync (
        PopupMenu::Options().withTargetComponent (exportReferenceAction.get()),
        [safeThis = Component::SafePointer<SpectrumCanvas> (this),
         captureId = reference->getCaptureId()] (int result)
        {
            if (safeThis == nullptr || result <= 0
                || safeThis->processor->getReferenceCaptureId() != captureId)
                return;
            safeThis->chooseReferenceFileToExport (static_cast<std::size_t> (result - 1));
        });
}

void SpectrumCanvas::chooseReferenceFileToExport (std::size_t channel)
{
    const auto reference = processor->getSpectrumReference();
    if (reference == nullptr || channel >= reference->getChannelCount())
        return;

    const auto suggested = getReferenceDirectory().getChildFile (
        "spectrum-baseline-ch"
        + String (reference->getSourceChannelIndices()[channel] + 1) + "-"
        + Time (reference->getCapturedAtUnixMilliseconds()).formatted ("%Y%m%d-%H%M%S")
        + ".json");
    referenceFileChooser = std::make_unique<FileChooser> ("Export a baseline",
                                                          suggested,
                                                          "*.json");
    referenceFileChooser->launchAsync (
        FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
            | FileBrowserComponent::warnAboutOverwriting,
        [this, channel, captureId = reference->getCaptureId()] (const FileChooser& chooser)
        {
            const auto file = chooser.getResult();
            if (file == File() || processor->getReferenceCaptureId() != captureId)
                return;

            referenceDirectory = file.getParentDirectory();
            processor->exportSpectrumReference (file, channel);
            updateStatus();
        });
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

    ColourGradient viridis (Colour::fromRGB (68, 1, 84), 0.0f, 0.0f,
                            Colour::fromRGB (253, 231, 37), 1.0f, 0.0f, false);
    viridis.addColour (0.33, Colour::fromRGB (49, 104, 142));
    viridis.addColour (0.66, Colour::fromRGB (53, 183, 121));
    for (std::size_t index = 0; index < spectrogramPalette.size(); ++index)
        spectrogramPalette[index] = viridis.getColourAtPosition (
            static_cast<double> (index) / static_cast<double> (spectrogramPalette.size() - 1));

    // Sized properly once the component has bounds; see resized().
    resizeSpectrogramImage();
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
    // The spectrogram does not impose the power spectrum's minimum size, so
    // this can be narrower than the legend and margins. Clamp rather than hand
    // the plot a negative size.
    plt->setBounds (20, 30,
                    std::max (0, getWidth() - legendWidth - 40),
                    std::max (0, getHeight() - 50));
    resizeSpectrogramImage();
    publishDisplayColumnCount();
    clearButton->setBounds (plt->getRight() - 80, plt->getY(), 60, 20);
    cursorLabel->setBounds (plt->getX() + 60, plt->getY(), 560, 20);
}

void CanvasPlot::publishDisplayColumnCount()
{
    // Before the first layout, or while squeezed to nothing, there is no area
    // to size to. Publishing a count anyway would have the worker reduce every
    // frame to a single column until the next resize, so keep the last one.
    const auto columns = displayType == SPECTROGRAM
                             ? (getSpectrogramArea().isEmpty() ? 0 : spectrogramImg->getHeight())
                             : plt->getDrawingWidth();
    if (columns > 0)
        processor->setDisplayColumnCount (static_cast<std::size_t> (columns));
}

Rectangle<int> CanvasPlot::getSpectrogramArea() const noexcept
{
    return getLocalBounds()
        .withTrimmedLeft (60)
        .withTrimmedRight (10)
        .withTrimmedTop (30)
        .withTrimmedBottom (10);
}

void CanvasPlot::resizeSpectrogramImage()
{
    const auto area = getSpectrogramArea();
    const auto scale = std::max (1.0f, Component::getApproximateScaleFactorForComponent (this));
    const auto height = std::max (1, roundToInt (static_cast<float> (area.getHeight()) * scale));
    if (spectrogramImg != nullptr && spectrogramImg->getHeight() == height)
        return;

    auto resizedImage = std::make_unique<Image> (Image::RGB, spectrogramHistoryColumns, height,
                                                 true, SoftwareImageType());
    if (spectrogramImg != nullptr)
    {
        // Keep the history rather than blanking it on every window drag. The
        // width is fixed, so this only rescales to the new frequency
        // resolution; every column stays the same frame.
        Graphics graphics (*resizedImage);
        graphics.drawImage (*spectrogramImg, resizedImage->getBounds().toFloat());
    }
    spectrogramImg = std::move (resizedImage);
}

void CanvasPlot::lookAndFeelChanged()
{
    plt->setBackgroundColour (findColour (ThemeColours::componentBackground));
    plt->setGridColour (findColour (ThemeColours::controlPanelText).withAlpha (0.5f));
    plt->setAxisColour (findColour (ThemeColours::controlPanelText));
    cursorLabel->setColour (Label::textColourId, findColour (ThemeColours::controlPanelText));

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
    plotFrequenciesStale = true;

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

    // The two displays want different column counts, and switching need not
    // change this component's bounds, so resized() cannot be relied on here.
    publishDisplayColumnCount();
    clear();
    repaint();
}

void CanvasPlot::plotPowerSpectrum (bool updateAutomaticRange)
{
    plt->clear();
    updateAmplitudeAxisLabel();

    // This runs at the refresh rate with up to four series per channel, so
    // nothing here may allocate in steady state: the axis is cached, and
    // plotLine() copies into storage the plot kept from the previous frame.
    refreshPlotFrequencies();
    const auto showingDelta = comparisonStatus.mode
                                  == spectrumviewer::SpectrumComparisonMode::deltaDb
                              && comparisonStatus.hasComparisonData();
    const auto showingAperiodic = ! comparisonStatus.hasComparisonData()
                                  && aperiodicDisplayMode
                                         == spectrumviewer::AperiodicDisplayMode::show;
    if (showingDelta)
        plt->plotLine (plotFrequencies,
                       zeroLine,
                       findColour (ThemeColours::controlPanelText),
                       1.0f,
                       0.5f);
    // activeChannels is bounded to MAX_CHANS by beginSpectrumFrame and by the
    // Channels parameter, but the trace and colour arrays are indexed directly
    // below, so make that bound explicit rather than assumed.
    const auto traceCount = std::min ({ static_cast<std::size_t> (activeChannels.size()),
                                        currPower.size(),
                                        chanColors.size() });
    const auto showingOverlay = comparisonStatus.mode
                                    == spectrumviewer::SpectrumComparisonMode::overlay
                                && comparisonStatus.hasComparisonData();
    // A baseline is the same spectrum behind every channel, so it is drawn
    // once, in no channel's colour, rather than stacked once per channel.
    if (showingOverlay && comparisonStatus.sharedAcrossChannels && traceCount > 0)
        plt->plotLine (plotFrequencies,
                       currComparison[0],
                       findColour (ThemeColours::controlPanelText),
                       1.5f,
                       0.6f);
    for (std::size_t i = 0; i < traceCount; i++)
    {
        if (showingDelta)
            plt->plotLine (plotFrequencies, currComparison[i], chanColors[i], 1.5f);
        else
        {
            if (showingOverlay && ! comparisonStatus.sharedAcrossChannels)
                plt->plotLine (plotFrequencies, currComparison[i], chanColors[i], 1.0f, 0.55f);
            if (showingAperiodic && currBaselineDb[i].size() == xvalues.size())
                plt->plotLine (plotFrequencies, currBaselineDb[i], chanColors[i], 2.0f, 0.55f);
            if (peakEnvelopeVisible)
                plt->plotLine (plotFrequencies, displayedPeakPower[i], chanColors[i], 1.0f, 0.35f);
            plt->plotLine (plotFrequencies, displayedPower[i], chanColors[i], 1.5f);
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
    // Every frame carries its frequencies, but they only differ when the range,
    // scale or plot width changed. Checking keeps the cached plot axis valid.
    if (channelIndex == 0
        && (xvalues.size() != valueCount
            || ! std::equal (xvalues.begin(), xvalues.end(), frequenciesHz)))
    {
        xvalues.assign (frequenciesHz, frequenciesHz + valueCount);
        plotFrequenciesStale = true;
    }
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
    // Only this channel's inputs changed. Rebuilding all of them here would
    // copy every channel's traces once per channel, per frame.
    rebuildDisplayedTrace (static_cast<std::size_t> (channelIndex));
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
    for (std::size_t channel = 0; channel < currPower.size(); ++channel)
        rebuildDisplayedTrace (channel);
}

void CanvasPlot::rebuildDisplayedTrace (std::size_t channel)
{
    if (channel >= currPower.size())
        return;

    const auto remove = aperiodicDisplayMode
                            == spectrumviewer::AperiodicDisplayMode::remove
                        && ! comparisonStatus.hasComparisonData();
    // Copy-assignment reuses the destination's capacity.
    displayedPower[channel] = currPower[channel];
    displayedPeakPower[channel] = currPeakPower[channel];
    if (! remove || currBaselineDb[channel].size() != currPower[channel].size())
        return;
    for (std::size_t bin = 0; bin < currPower[channel].size(); ++bin)
    {
        displayedPower[channel][bin] -= currBaselineDb[channel][bin];
        displayedPeakPower[channel][bin] -= currBaselineDb[channel][bin];
    }
}

void CanvasPlot::refreshPlotFrequencies()
{
    if (! plotFrequenciesStale && plotFrequenciesScale == frequencyScale)
        return;

    FrequencyPlot::transformFrequencies (xvalues, frequencyScale, plotFrequencies);
    zeroLine.assign (xvalues.size(), 0.0f);
    plotFrequenciesScale = frequencyScale;
    plotFrequenciesStale = false;
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
        // Compared in place: the legend is the same on almost every frame, and
        // building an Array to compare against would allocate on each one.
        auto legendChanged = activeStreamId != sourceStreamId
                             || static_cast<std::size_t> (activeChannels.size())
                                    != frameChannelCount;
        for (std::size_t channel = 0; ! legendChanged && channel < frameChannelCount; ++channel)
            legendChanged = activeChannels.getUnchecked (static_cast<int> (channel))
                            != sourceChannelIndices[channel];

        if (legendChanged)
        {
            activeChannels.clearQuick();
            for (std::size_t channel = 0; channel < frameChannelCount; ++channel)
                activeChannels.add (sourceChannelIndices[channel]);
            activeStreamId = sourceStreamId;
            repaint();
        }
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

    const auto imageWidth = spectrogramImg->getWidth();
    const auto imageHeight = spectrogramImg->getHeight();

    // Scroll the history one column right; the newest column goes at x = 0.
    spectrogramImg->moveImageSection (1, 0, 0, 0, imageWidth - 1, imageHeight);

    // Only the first selected channel is drawn; paint() says so on screen.
    const auto& channelDb = displayedPower[0];
    const auto lastBin = static_cast<int> (channelDb.size()) - 1;
    const auto lastRow = std::max (1, imageHeight - 1);
    const auto lastColour = static_cast<float> (spectrogramPalette.size() - 1);

    // One BitmapData for the whole column. setPixelAt() constructs one per
    // call, which at a row per physical pixel was most of this function.
    Image::BitmapData column (*spectrogramImg, 0, 0, 1, imageHeight,
                              Image::BitmapData::writeOnly);
    for (auto y = 0; y < imageHeight; ++y)
    {
        // Row 0 is the top of the image and so shows the highest column. The
        // reduced columns are already spaced for the frequency scale, so a
        // linear row-to-column map is correct for both linear and log axes.
        const auto fromBottom = static_cast<float> (lastRow - y) / static_cast<float> (lastRow);
        const auto dataIndex = jlimit (0, lastBin, roundToInt (fromBottom * static_cast<float> (lastBin)));
        const auto valueDb = channelDb[static_cast<std::size_t> (dataIndex)];
        const auto level = std::isfinite (valueDb)
                               ? jlimit (0.0f, 1.0f,
                                         (valueDb - colourRange.minimum) / colourSpan)
                               : 0.0f;
        column.setPixelColour (0, y,
                               spectrogramPalette[static_cast<std::size_t> (
                                   roundToInt (level * lastColour))]);
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

            // paint() must never throw: an exception escaping a JUCE paint call
            // terminates the host. Wrap rather than index past the palette.
            g.setColour (chanColors[static_cast<std::size_t> (i) % chanColors.size()]);
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

        const auto area = getSpectrogramArea();
        const auto axisX = static_cast<float> (area.getX() - 13);
        const auto areaTop = static_cast<float> (area.getY());
        const auto areaBottom = static_cast<float> (area.getBottom());
        const auto tickLabelWidth = area.getX() - 25;
        constexpr int tickLabelHeight = 20;

        // Only the first selected channel is drawn, and the legend is not, so
        // name it here rather than leave the selection to imply all of them.
        if (! activeChannels.isEmpty())
        {
            g.setFont (FontOptions ("Inter", "Regular", 13.0f));
            g.drawText ((hasFrameTiming ? processor->getChanName (activeStreamId, activeChannels[0])
                                        : processor->getChanName (activeChannels[0]))
                            + (activeChannels.size() > 1 ? " (first selected channel)" : ""),
                        area.getX(), area.getY() - 25, area.getWidth(), 20,
                        Justification::centredLeft, true);
        }

        g.drawLine (axisX, areaTop, axisX, areaBottom, 2.0f);

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
            const auto ytickloc = areaBottom - fraction * (areaBottom - areaTop);
            g.drawLine (axisX - 10.0f, ytickloc, axisX, ytickloc, 2.0f);

            const auto yTick = frequency >= 1000.0f
                                   ? String (frequency / 1000.0f, 0) + "k"
                                   : String (frequency, 0);

            g.drawText (yTick,
                        0,
                        roundToInt (ytickloc) - tickLabelHeight / 2,
                        tickLabelWidth,
                        tickLabelHeight,
                        Justification::right,
                        false);
        }

        // Stretched across the area, so the scroll speed follows the window
        // width. Vertically the image already matches the area's physical
        // pixels; see resizeSpectrogramImage().
        g.drawImage (*spectrogramImg, area.toFloat());
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
