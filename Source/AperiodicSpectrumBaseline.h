/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef APERIODIC_SPECTRUM_BASELINE_H_INCLUDED
#define APERIODIC_SPECTRUM_BASELINE_H_INCLUDED

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
enum class AperiodicDisplayMode
{
    off = 1,
    show = 2,
    remove = 3
};

/**
    Robust, allocation-free estimate of the broad spectral background.

    The estimator takes medians in narrow log-frequency bands, smooths those
    knots, and interpolates in log frequency. Narrow positive lines therefore
    remain above the estimate instead of pulling it toward their peaks.
*/
class AperiodicSpectrumBaseline
{
public:
    AperiodicSpectrumBaseline (std::size_t maximumInputBins,
                               std::size_t maximumOutputBins);

    /** Fits each channel of a full one-sided planarPsd, numBins per channel,
        and evaluates the fit in dB at outputFrequenciesHz into
        planarBaselineDb, outputBinCount per channel.

        Returns false for invalid arguments, including a sample rate or window
        that leaves no band between 10 Hz (or two bins) and Nyquist to fit;
        nothing is written then. Otherwise every
        output value is written. A value is NaN where there is no fit: a
        non-finite frequency, one outside the fitted band, or a channel with too
        little positive power to fit, which does not affect the other channels.

        outputFrequenciesHz may be in any order. Ascending order, as the
        display reducer produces, is the fast path. */
    bool estimate (const float* planarPsd,
                   std::size_t numChannels,
                   std::size_t numBins,
                   double sampleRateHz,
                   std::size_t windowSampleCount,
                   const float* outputFrequenciesHz,
                   std::size_t outputBinCount,
                   float* planarBaselineDb) noexcept;

private:
    static constexpr std::size_t maximumKnots = 64;
    std::size_t inputCapacity;
    std::size_t outputCapacity;
    std::vector<float> bandScratch;
    std::vector<float> knotLogFrequencies;
    std::vector<float> knotValuesDb;
    std::vector<float> smoothedKnotValuesDb;
};
} // namespace spectrumviewer

#endif // APERIODIC_SPECTRUM_BASELINE_H_INCLUDED
