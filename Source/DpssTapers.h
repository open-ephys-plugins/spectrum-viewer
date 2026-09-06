/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef DPSS_TAPERS_H_INCLUDED
#define DPSS_TAPERS_H_INCLUDED

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
enum class DpssGenerationStatus
{
    success,
    invalidSampleCount,
    invalidTimeHalfBandwidth,
    invalidTaperCount,
    eigenSolverFailure,
    concentrationFailure,
    allocationFailure
};

/** Immutable, length-N symmetric DPSS taper bank.

    Tapers are L2-normalized and stored taper-major as floats for the production
    DSP path. Concentration ratios retain double precision for taper selection
    and adaptive weighting. Generate banks off real-time threads.
*/
struct DpssTaperBank
{
    DpssGenerationStatus status = DpssGenerationStatus::eigenSolverFailure;
    int eigenSolverInfo = 0;
    std::size_t sampleCount = 0;
    std::size_t taperCount = 0;
    double timeHalfBandwidth = 0.0;
    std::vector<float> tapers;
    std::vector<double> concentrationRatios;

    bool succeeded() const noexcept;
    const float* getTaper (std::size_t taperIndex) const noexcept;
};

/** Generates the leading length-N DPSS tapers and their concentration ratios. */
DpssTaperBank generateDpssTapers (std::size_t sampleCount,
                                  double timeHalfBandwidth,
                                  std::size_t taperCount,
                                  unsigned int fftPlanningFlags = 1U << 6U /* FFTW_ESTIMATE */) noexcept;

const char* getDpssGenerationStatusDescription (DpssGenerationStatus status) noexcept;
} // namespace spectrumviewer

#endif // DPSS_TAPERS_H_INCLUDED
