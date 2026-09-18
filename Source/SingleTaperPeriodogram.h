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

#ifndef SINGLE_TAPER_PERIODOGRAM_H_INCLUDED
#define SINGLE_TAPER_PERIODOGRAM_H_INCLUDED

#include "SpectrumEstimation.h"

#include <OpenEphysFFTWBatch.h>

#include <cstddef>
#include <memory>
#include <vector>

namespace spectrumviewer
{
/**
    Allocation-free float single-taper periodogram using a reusable FFTW plan.

    Construction and plan creation must occur off real-time threads. compute()
    accepts circular-history windows without first making a contiguous copy;
    detrending and tapering materialize directly into aligned FFT input.
*/
class SingleTaperPeriodogram
{
public:
    SingleTaperPeriodogram (std::size_t numChannels,
                            std::size_t numSamples,
                            double sampleRate,
                            std::vector<float> taper,
                            DetrendMode detrendMode,
                            unsigned int planningFlags = 0U /* FFTW_MEASURE */);

    /** Returns false for malformed views or non-finite input; output is then stale. */
    bool compute (const ChannelSampleView* channels, std::size_t numChannels) noexcept;

    const float* getChannelData (std::size_t channel) const noexcept
    {
        return psd.data() + channel * binCount;
    }

    std::size_t getChannelCount() const noexcept { return channelCount; }
    std::size_t getSampleCount() const noexcept { return sampleCount; }
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
    std::size_t binCount;
    double sampleRateHz;
    double normalization;
    double centredTimeSquareSum;
    DetrendMode mode;
    std::vector<float> taperCoefficients;
    std::vector<float> psd;
    std::unique_ptr<FFTWRealToComplexBatchFloat> transform;
};
} // namespace spectrumviewer

#endif // SINGLE_TAPER_PERIODOGRAM_H_INCLUDED
