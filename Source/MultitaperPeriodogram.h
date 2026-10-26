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

#ifndef MULTITAPER_PERIODOGRAM_H_INCLUDED
#define MULTITAPER_PERIODOGRAM_H_INCLUDED

#include "DpssTapers.h"
#include "SpectrumEstimation.h"

#include <OpenEphysFFTWBatch.h>

#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace spectrumviewer
{
/**
    Allocation-free, equal-weighted multitaper PSD estimator.

    Construction and FFTW planning must occur off real-time threads. compute()
    estimates each channel's trend once, materializes every tapered row directly
    into transform-major FFTW storage, and averages calibrated eigenspectra in
    linear power. It performs no line detection, adaptive weighting, smoothing,
    or temporal averaging. Distinct instances may execute concurrently; one
    instance must be confined to one analysis thread.
*/
class MultitaperPeriodogram
{
public:
    MultitaperPeriodogram (std::size_t numChannels,
                           double sampleRate,
                           std::shared_ptr<const DpssTaperBank> taperBank,
                           DetrendMode detrendMode,
                           unsigned int planningFlags = 0U /* FFTW_MEASURE */);

    /** Returns false for malformed views or non-finite input; output is then stale. */
    bool compute (const ChannelSampleView* channels, std::size_t numChannels) noexcept;

    const float* getChannelData (std::size_t channel) const noexcept;
    const DpssTaperBank& getTaperBank() const noexcept { return *bank; }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getSampleCount() const noexcept { return sampleCount; }
    std::size_t getTaperCount() const noexcept { return taperCount; }
    std::size_t getBinCount() const noexcept { return binCount; }
    double getSampleRate() const noexcept { return sampleRateHz; }
    double getBinWidth() const noexcept { return sampleRateHz / static_cast<double> (sampleCount); }
    double getFrequency (std::size_t bin) const noexcept
    {
        return static_cast<double> (bin) * getBinWidth();
    }

private:
    std::size_t channelCount;
    std::size_t sampleCount;
    std::size_t taperCount;
    std::size_t binCount;
    double sampleRateHz;
    double centredTimeSquareSum;
    DetrendMode mode;
    std::shared_ptr<const DpssTaperBank> bank;
    std::vector<const float*> taperPointers;
    std::vector<double> inverseNormalizations;
    std::vector<float> psd;
    std::vector<float> workingPsd;
    std::vector<float> detrendedTile;
    std::unique_ptr<FFTWRealToComplexBatchFloat> transform;
    std::vector<float*> fftInputs;
    std::vector<const std::complex<float>*> fftOutputs;
};
} // namespace spectrumviewer

#endif // MULTITAPER_PERIODOGRAM_H_INCLUDED
