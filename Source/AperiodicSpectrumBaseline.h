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
