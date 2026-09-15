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

#include "AperiodicSpectrumBaseline.h"
#include "SpectrumAmplitudeRange.h"
#include "SpectrumDisplaySettings.h"
#include "SpectrumViewer.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

class SpectrumCanvas;

// Component for housing power spectrum & spectrograph plots
class TESTABLE FrequencyPlot : public InteractivePlot
{
public:
    FrequencyPlot();

    void plot (std::vector<float> x,
               std::vector<float> y,
               Colour colour = Colours::white,
               float width = 1.0f,
               float opacity = 1.0f,
               PlotType type = PlotType::LINE) override;
    void clear();

    void setFrequencyAxis (spectrumviewer::FrequencyScale scale,
                           float minimumHz,
                           float maximumHz,
                           float minimumDb,
                           float maximumDb);

    static std::vector<float> transformFrequencies (
        const std::vector<float>& frequencies,
        spectrumviewer::FrequencyScale scale);
    float frequencyAt (Point<int> point) const noexcept;
    int getDrawingWidth() const noexcept { return drawComponent->getWidth(); }
    void paintOverChildren (Graphics& graphics) override;
    void resized() override;

#if BUILD_TESTS
    Rectangle<int> getDrawingBoundsForTesting() const { return drawComponent->getBounds(); }
    String getXAxisLabelForTesting() const { return xLabel->getText(); }
    String getYAxisLabelForTesting() const { return yLabel->getText(); }
    XYRange getRangeForTesting()
    {
        XYRange range;
        getRange (range);
        return range;
    }
#endif

private:
    struct LineSeries
    {
        std::vector<float> x;
        std::vector<float> y;
        Colour colour;
        float width = 1.0f;
        float opacity = 1.0f;
    };

    std::vector<LineSeries> lineSeries;
    spectrumviewer::FrequencyScale frequencyScale = spectrumviewer::FrequencyScale::linear;
    float minimumFrequencyHz = 0.0f;
    float maximumFrequencyHz = 1.0f;
    float minimumAmplitude = -60.0f;
    float maximumAmplitude = 60.0f;
    std::vector<float> logarithmicTickPositions;
    std::vector<String> logarithmicTickLabels;
};

class TESTABLE CanvasPlot : public Component,
                            public Button::Listener
{
public:
    /** Constructor */
    CanvasPlot (SpectrumViewer* p);

    /** Destructor */
    ~CanvasPlot() {}

    /** Draws the canvas */
    void paint (Graphics& g) override;

    /** Updates component boundaries */
    void resized() override;

    /** Called when the look and feel changes */
    void lookAndFeelChanged() override;

    void updateActiveChans();

    void setFrequencyRange (int freqStart, int freqEnd, float freqStep);

    void setBinWidth (float newBinWidth);

    void updatePowerSpectrum (const float* meanPsd,
                              const float* peakPsd,
                              std::size_t valueCount,
                              const float* frequenciesHz,
                              const String& unit,
                              spectrumviewer::FrequencyScale scale,
                              double minimumFrequencyHz,
                              double maximumFrequencyHz,
                              int channelIndex,
                              const float* baselineDb = nullptr,
                              const float* comparisonData = nullptr,
                              spectrumviewer::SpectrumComparisonFrameStatus comparison = {});

    void setAmplitudeDisplay (SpectrumAmplitudeDisplay display);
    void setAperiodicDisplayMode (spectrumviewer::AperiodicDisplayMode mode);
    void setPeakEnvelopeVisible (bool shouldBeVisible);

    void setAmplitudeRangeMode (spectrumviewer::AmplitudeRangeMode mode);
    bool setFixedAmplitudeRange (float minimumDb, float maximumDb);
    spectrumviewer::AmplitudeRangeMode getAmplitudeRangeMode() const noexcept;
    spectrumviewer::DecibelRange getAmplitudeRange() const noexcept;
    bool hasAutomaticAmplitudeRange() const noexcept;

    void beginSpectrumFrame (std::uint64_t configurationGeneration,
                             std::uint64_t sequence,
                             std::size_t channelCount,
                             double hopDurationSeconds,
                             std::uint16_t sourceStreamId = 0,
                             const int* sourceChannelIndices = nullptr);

    void mouseMove (const MouseEvent& event) override;

    void plotPowerSpectrum (bool updateAutomaticRange = false);

    void drawSpectrogram();

    /** Sets the display type for the canvas (Power Spectrum or Spectrogram)*/
    void setDisplayType (DisplayType type);

    /** Called when a button is clicked */
    void buttonClicked (Button* button) override;

    /** Clears the plot */
    void clear();

#if BUILD_TESTS
    std::size_t getFrequencyCountForTesting() const noexcept { return xvalues.size(); }
    const std::vector<float>& getFrequenciesForTesting() const noexcept { return xvalues; }
    const std::vector<float>& getMeanTraceForTesting (std::size_t channel) const
    {
        return currLinearPower.at (channel);
    }
    const std::vector<float>& getPeakTraceForTesting (std::size_t channel) const
    {
        return currLinearPeakPower.at (channel);
    }
    std::vector<float> getPlottedFrequenciesForTesting() const
    {
        return FrequencyPlot::transformFrequencies (xvalues, frequencyScale);
    }
    const std::vector<float>& getDbMeanTraceForTesting (std::size_t channel) const
    {
        return currPower.at (channel);
    }
    const std::vector<float>& getDisplayedDbMeanTraceForTesting (std::size_t channel) const
    {
        return displayedPower.at (channel);
    }
    const std::vector<float>& getComparisonTraceForTesting (std::size_t channel) const
    {
        return currComparison.at (channel);
    }
    spectrumviewer::SpectrumComparisonMode getComparisonModeForTesting() const noexcept
    {
        return comparisonStatus.mode;
    }
    float getMinimumFrequencyForTesting() const noexcept { return displayMinimumFrequencyHz; }
    float getMaximumFrequencyForTesting() const noexcept { return displayMaximumFrequencyHz; }
    spectrumviewer::FrequencyScale getFrequencyScaleForTesting() const noexcept
    {
        return frequencyScale;
    }
    String getXAxisLabelForTesting() const { return plt->getXAxisLabelForTesting(); }
    String getYAxisLabelForTesting() const { return plt->getYAxisLabelForTesting(); }
    XYRange getPlotRangeForTesting() { return plt->getRangeForTesting(); }
    Rectangle<int> getDrawingBoundsForTesting() const
    {
        return plt->getDrawingBoundsForTesting().translated (plt->getX(), plt->getY());
    }
    double getPendingRangeElapsedSecondsForTesting() const noexcept
    {
        return pendingRangeElapsedSeconds;
    }
    Array<int> getActiveChannelsForTesting() const { return activeChannels; }
    std::uint16_t getActiveStreamForTesting() const noexcept { return activeStreamId; }
#endif

    int legendWidth = 150;

    DisplayType displayType;

private:
    void updateAmplitudeAxisLabel();
    void rebuildDisplayedTraces();

    std::vector<Colour> chanColors = { Colour (200, 200, 200),
                                       Colour (230, 159, 0),
                                       Colour (86, 180, 233),
                                       Colour (0, 158, 115),
                                       Colour (240, 228, 66),
                                       Colour (0, 114, 178),
                                       Colour (242, 66, 53),
                                       Colour (204, 121, 167) };

    std::unique_ptr<UtilityButton> clearButton;

    SpectrumViewer* processor;

    int rowHeight = 50;

    std::vector<std::vector<float>> currPower; // channels x freqs
    std::vector<std::vector<float>> currPeakPower;
    std::vector<std::vector<float>> currBaselineDb;
    std::vector<std::vector<float>> displayedPower;
    std::vector<std::vector<float>> displayedPeakPower;
    std::vector<std::vector<float>> currLinearPower;
    std::vector<std::vector<float>> currLinearPeakPower;
    std::vector<std::vector<float>> currComparison;
    std::vector<String> channelUnits;
    spectrumviewer::SpectrumAmplitudeRange amplitudeRange;
    bool amplitudeUnitsChanged = false;
    spectrumviewer::SpectrumComparisonFrameStatus comparisonStatus;

    std::vector<float> xvalues;

    std::unique_ptr<FrequencyPlot> plt;
    std::unique_ptr<Label> cursorLabel;

    float freqStep;
    int freqStart = 0;
    int nFreqs;
    int freqEnd;

    Array<int> activeChannels;
    std::uint16_t activeStreamId = 0;
    SpectrumAmplitudeDisplay amplitudeDisplay = SpectrumAmplitudeDisplay::psd;
    spectrumviewer::AperiodicDisplayMode aperiodicDisplayMode =
        spectrumviewer::AperiodicDisplayMode::off;
    bool peakEnvelopeVisible = true;
    spectrumviewer::FrequencyScale frequencyScale = spectrumviewer::FrequencyScale::linear;
    float displayMinimumFrequencyHz = 0.0f;
    float displayMaximumFrequencyHz = 1000.0f;
    std::size_t frameChannelCount = 0;
    std::uint64_t lastConfigurationGeneration = 0;
    std::uint64_t lastFrameSequence = 0;
    double pendingRangeElapsedSeconds = 0.0;
    bool hasFrameTiming = false;

    /** Image to draw*/
    std::unique_ptr<Image> spectrogramImg;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CanvasPlot);
};

/** 

	Draws the real-time power spectrum

*/
class TESTABLE SpectrumCanvas : public Visualizer,
                       public ComboBox::Listener,
                       public Slider::Listener,
                       public Button::Listener
{
public:
    /** Constructor.

        settings outlives the canvas: it is a member of the editor, which
        destroys the canvas before its own members.
    */
    SpectrumCanvas (SpectrumViewer* n, SpectrumDisplaySettings& settings);

    /** Destructor */
    ~SpectrumCanvas() override;

    /** Called when tab becomes visible again */
    void refreshState() override;

    /** Updates settings */
    void updateSettings() override;

    /** Called when data acquisition begins */
    void beginAnimation() override;

    /** Called when data acquisition ends */
    void endAnimation() override;

    /** Called instead of repaint to avoid re-painting sub-components*/
    void refresh() override;

    /** Draws the canvas */
    void paint (Graphics& g) override;

    /** Updates component boundaries */
    void resized() override;

    /** Pushes every stored display setting into the controls, the plot and the
        processor. Called at construction and whenever a session is loaded. */
    void applyDisplaySettings();

    /** Opens or closes the display-options drawer at the bottom of the canvas. */
    void setOptionsDrawerOpen (bool shouldBeOpen);

    bool isOptionsDrawerOpen() const noexcept { return displaySettings.optionsDrawerOpen; }

    CanvasPlot* getPlotPtr() { return canvasPlot.get(); };

#if BUILD_TESTS
    Component* getMainOptionsBarForTesting() const noexcept { return mainOptionsBar.get(); }
    Component* getOptionsDrawerForTesting() const noexcept { return optionsDrawer.get(); }
    Component* getOptionsContentForTesting() const noexcept { return optionsContent.get(); }
    Component* getOptionsViewportForTesting() const noexcept { return optionsViewport.get(); }
    Component* getViewportForTesting() const noexcept { return viewport.get(); }
#endif

    void comboBoxChanged (ComboBox* comboBox) override;
    void sliderValueChanged (Slider* slider) override;
    void buttonClicked (Button* button) override;

private:
    /** Visualizer already owns a Timer, driving refresh() at the plot rate and
        only while acquisition runs. Capture and reference status must keep
        updating when it is not, so this drives that at a slower rate. */
    class StatusTimer final : public Timer
    {
    public:
        explicit StatusTimer (SpectrumCanvas& ownerToUse) : owner (ownerToUse) {}
        void timerCallback() override { owner.updateStatus(); }

    private:
        SpectrumCanvas& owner;
    };

    // The options bars keep their natural width and scroll horizontally when
    // the canvas is narrower, so every height here is fixed and known.
    static constexpr int controlHeight = 22;
    static constexpr int controlSpacing = 12;
    static constexpr int groupSpacing = 10;
    static constexpr int groupPadding = 8;
    // Room the group outline leaves for the title it draws inline at the top.
    static constexpr int groupTitleHeight = 14;
    static constexpr int groupRowGap = 6;
    static constexpr int barPadding = 10;
    static constexpr int optionsButtonWidth = 64;
    static constexpr int scrollBarThickness = 12;
    // A group is a title over two rows of controls; the main bar is one row.
    static constexpr int groupHeight =
        groupTitleHeight + 2 * controlHeight + groupRowGap + groupPadding;
    static constexpr int optionsBarHeight = 2 * barPadding + controlHeight;
    static constexpr int optionsDrawerHeight = 2 * barPadding + groupHeight;

    /** One titled group of related controls in the options drawer.

        The contents sit on two rows rather than one, which roughly halves how
        wide the group has to be and so fits far more into a narrow canvas
        before the drawer has to scroll. Controls that hide themselves - the
        fixed-range sliders - drop out of both the measured width and the
        layout, and a group with nothing showing takes no space at all. */
    class ControlGroup final : public GroupComponent
    {
    public:
        static constexpr int rowCount = 2;

        explicit ControlGroup (const String& titleText);

        /** Adds a label/control pair to one of the group's rows. A null label
            gives a bare control. */
        void addPair (int row, Label* label, Component* control,
                      int labelWidth, int controlWidth);

        /** Width the visible contents need. Zero when nothing is showing. */
        int getPreferredWidth() const;

        void lookAndFeelChanged() override;
        void resized() override;

    private:
        struct Entry
        {
            int row;
            Label* label;
            Component* control;
            int labelWidth;
            int controlWidth;

            bool isShowing() const { return control != nullptr && control->isVisible(); }
            int width() const { return labelWidth + controlWidth; }
        };

        /** Width each row's visible entries need, gaps included. */
        std::array<int, rowCount> getRowWidths() const;

        std::vector<Entry> entries;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlGroup);
    };

    void updateStatus();

    void setDisplayType (DisplayType type);
    void createControls();
    void createControlGroups();

    /** Positions the options area and returns the height it occupies. */
    int layOutControls();
    void updateAmplitudeRangeControls();
    void applyAmplitudeRangeToPlot();
    void applyFrequencyRange();
    void refreshNyquistRangeItem();

    SpectrumViewer* processor;
    SpectrumDisplaySettings& displaySettings;

    std::unique_ptr<Viewport> viewport;
    std::unique_ptr<CanvasPlot> canvasPlot;

    // The options area keeps its natural width and scrolls horizontally rather
    // than compressing controls, so both bars live inside one viewport. The
    // drawer sits above the main bar and they scroll together.
    std::unique_ptr<Viewport> optionsViewport;
    std::unique_ptr<Component> optionsContent;
    std::unique_ptr<Component> mainOptionsBar;
    std::unique_ptr<Component> optionsDrawer;
    std::unique_ptr<Button> showHideOptionsButton;

    std::unique_ptr<Label> displayLabel;
    std::unique_ptr<ComboBox> displayType;
    std::unique_ptr<Label> frequencyLabel;
    std::unique_ptr<ComboBox> frequencyRange;
    std::unique_ptr<Label> profileLabel;
    std::unique_ptr<ComboBox> analysisProfile;
    std::unique_ptr<Label> scaleLabel;
    std::unique_ptr<ComboBox> frequencyScale;
    std::unique_ptr<Label> amplitudeLabel;
    std::unique_ptr<ComboBox> amplitudeDisplay;
    std::unique_ptr<Label> baselineLabel;
    std::unique_ptr<ComboBox> baselineDisplay;
    std::unique_ptr<ToggleButton> peakEnvelope;
    std::unique_ptr<Label> amplitudeRangeLabel;
    std::unique_ptr<ComboBox> amplitudeRangeMode;
    std::unique_ptr<Label> minimumDbLabel;
    std::unique_ptr<Slider> minimumDb;
    std::unique_ptr<Label> maximumDbLabel;
    std::unique_ptr<Slider> maximumDb;
    std::unique_ptr<Label> automaticRangeLabel;
    std::unique_ptr<Label> captureDurationLabel;
    std::unique_ptr<ComboBox> captureDuration;
    std::unique_ptr<UtilityButton> captureAction;
    std::unique_ptr<Label> captureStatusLabel;
    std::unique_ptr<Label> comparisonModeLabel;
    std::unique_ptr<ComboBox> comparisonMode;
    std::unique_ptr<UtilityButton> setReferenceAction;
    std::unique_ptr<UtilityButton> clearReferenceAction;
    std::unique_ptr<Label> referenceStatusLabel;

    // The drawer's controls belong to these, in the order they are laid out.
    std::vector<std::unique_ptr<ControlGroup>> controlGroups;

    Array<Range<int>> freqRanges;
    StatusTimer statusTimer { *this };

    DisplayType currentDisplayType = POWER_SPECTRUM;
    bool unavailableStateCleared = false;
    bool applyingSettings = false;

    // Setting a reference is asynchronous, so a request the worker could not
    // apply surfaces here rather than at the click. Latched until the next
    // reference is set or cleared, so the message is not lost between polls.
    std::uint64_t seenDroppedReferenceRequests = 0;
    bool referenceRequestWasDropped = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumCanvas);
};

#endif // SPECTRUMCANVAS_H_INCLUDED
