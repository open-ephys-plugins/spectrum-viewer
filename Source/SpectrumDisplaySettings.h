/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

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

#ifndef SPECTRUM_DISPLAY_SETTINGS_H_INCLUDED
#define SPECTRUM_DISPLAY_SETTINGS_H_INCLUDED

#include <VisualizerEditorHeaders.h>

#include <cmath>

/**
    Display-only state for the Spectrum Viewer, as plain values.

    The controls that edit these live on the canvas, but the canvas is created
    lazily and may never be created at all if the visualizer is never opened.
    The editor therefore owns the values, so that saving and loading a signal
    chain works either way; the canvas is only a view over them.

    Values are the one-based ComboBox ids the controls use, which is also what
    is written to the session XML. Keep the defaults here and nowhere else.
*/
struct SpectrumDisplaySettings
{
    static constexpr double defaultMinimumDb = -25.0;
    static constexpr double defaultMaximumDb = 25.0;

    /** Fixed ranges narrower than this are not useful to read. */
    static constexpr double minimumSpanDb = 20.0;

    int displayTypeId = 1; // 1 power spectrum, 2 spectrogram
    int frequencyRangeId = 4; // 1..3 fixed bands, 4 full to Nyquist
    int analysisProfileId = 1; // 1 fast, 2 balanced, 3 fine
    int frequencyScaleId = 1; // 1 linear, 2 logarithmic
    int amplitudeDisplayId = 1; // 1 PSD, 2 ASD
    int aperiodicDisplayId = 1; // 1 off, 2 show fit, 3 remove
    int amplitudeRangeModeId = 1; // 1 automatic, 2 fixed
    int captureDurationId = 1; // 1 ten seconds, 2 thirty, 3 sixty
    int comparisonModeId = 1; // 1 absolute, 2 overlay, 3 delta dB
    bool peakEnvelopeVisible = true;
    bool optionsDrawerOpen = false;
    double minimumDb = defaultMinimumDb;
    double maximumDb = defaultMaximumDb;

    /** Clamps every field to a usable value.

        Session XML is user-editable and can also come from an older version,
        so nothing here may be trusted to be in range.
    */
    void sanitize() noexcept
    {
        displayTypeId = jlimit (1, 2, displayTypeId);
        frequencyRangeId = jlimit (1, 4, frequencyRangeId);
        analysisProfileId = jlimit (1, 3, analysisProfileId);
        frequencyScaleId = jlimit (1, 2, frequencyScaleId);
        amplitudeDisplayId = jlimit (1, 2, amplitudeDisplayId);
        aperiodicDisplayId = jlimit (1, 3, aperiodicDisplayId);
        amplitudeRangeModeId = jlimit (1, 2, amplitudeRangeModeId);
        captureDurationId = jlimit (1, 3, captureDurationId);
        comparisonModeId = jlimit (1, 3, comparisonModeId);

        if (! std::isfinite (minimumDb) || ! std::isfinite (maximumDb)
            || maximumDb - minimumDb < minimumSpanDb)
        {
            minimumDb = defaultMinimumDb;
            maximumDb = defaultMaximumDb;
        }
    }

    void writeTo (XmlElement& xml) const
    {
        xml.setAttribute ("display_type", displayTypeId);
        xml.setAttribute ("frequency_range", frequencyRangeId);
        xml.setAttribute ("analysis_profile", analysisProfileId);
        xml.setAttribute ("frequency_scale", frequencyScaleId);
        xml.setAttribute ("amplitude_display", amplitudeDisplayId);
        xml.setAttribute ("aperiodic_display", aperiodicDisplayId);
        xml.setAttribute ("show_peak_envelope", peakEnvelopeVisible);
        xml.setAttribute ("amplitude_range_mode", amplitudeRangeModeId);
        xml.setAttribute ("minimum_db", minimumDb);
        xml.setAttribute ("maximum_db", maximumDb);
        xml.setAttribute ("capture_duration", captureDurationId);
        xml.setAttribute ("comparison_mode", comparisonModeId);
        xml.setAttribute ("options_drawer_open", optionsDrawerOpen);
    }

    void readFrom (const XmlElement& xml)
    {
        const SpectrumDisplaySettings defaults;
        displayTypeId = xml.getIntAttribute ("display_type", defaults.displayTypeId);
        frequencyRangeId = xml.getIntAttribute ("frequency_range", defaults.frequencyRangeId);
        analysisProfileId = xml.getIntAttribute ("analysis_profile", defaults.analysisProfileId);
        frequencyScaleId = xml.getIntAttribute ("frequency_scale", defaults.frequencyScaleId);
        amplitudeDisplayId = xml.getIntAttribute ("amplitude_display", defaults.amplitudeDisplayId);
        aperiodicDisplayId = xml.getIntAttribute ("aperiodic_display", defaults.aperiodicDisplayId);
        peakEnvelopeVisible = xml.getBoolAttribute ("show_peak_envelope",
                                                    defaults.peakEnvelopeVisible);
        amplitudeRangeModeId = xml.getIntAttribute ("amplitude_range_mode",
                                                    defaults.amplitudeRangeModeId);
        minimumDb = xml.getDoubleAttribute ("minimum_db", defaults.minimumDb);
        maximumDb = xml.getDoubleAttribute ("maximum_db", defaults.maximumDb);
        captureDurationId = xml.getIntAttribute ("capture_duration", defaults.captureDurationId);
        comparisonModeId = xml.getIntAttribute ("comparison_mode", defaults.comparisonModeId);
        optionsDrawerOpen = xml.getBoolAttribute ("options_drawer_open",
                                                  defaults.optionsDrawerOpen);
        sanitize();
    }
};

#endif // SPECTRUM_DISPLAY_SETTINGS_H_INCLUDED
