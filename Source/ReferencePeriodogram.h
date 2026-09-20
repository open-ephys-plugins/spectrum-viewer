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

#ifndef REFERENCE_PERIODOGRAM_H_INCLUDED
#define REFERENCE_PERIODOGRAM_H_INCLUDED

#include "SpectrumEstimation.h"

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
struct PeriodogramResult
{
    const double* getChannelData (std::size_t channel) const noexcept
    {
        return psd.data() + channel * numBins;
    }

    double getFrequency (std::size_t bin) const noexcept
    {
        return static_cast<double> (bin) * binWidth;
    }

    std::size_t numChannels = 0;
    std::size_t numSamples = 0;
    std::size_t numBins = 0;
    double sampleRate = 0.0;
    double binWidth = 0.0;
    std::vector<double> psd;
};

/**
    Correctness-first single-taper periodogram for tests and offline validation.

    Float input matches the host sample type, while detrending, tapering, DFT
    accumulation, and output use double precision to give the test oracle
    numerical headroom. This implementation intentionally evaluates the DFT
    directly in O(N^2) time and must not be used by the real-time worker.
    Optimized float FFT implementations are expected to match its output within
    an explicitly tested tolerance.
*/
class ReferencePeriodogram
{
public:
    /**
        Computes a one-sided PSD for planar input channels.

        channelStride is the distance between channel starts and must be at least
        numSamples. taper must contain numSamples coefficients. Output units are
        input-units squared per Hz, normalized as |X[k]|^2 divided by sample rate
        and taper energy. Interior one-sided bins are doubled; DC and an even-N
        Nyquist bin are not.
    */
    static PeriodogramResult compute (const float* planarSamples,
                                      std::size_t numChannels,
                                      std::size_t channelStride,
                                      std::size_t numSamples,
                                      double sampleRate,
                                      const std::vector<double>& taper,
                                      DetrendMode detrendMode);
};
} // namespace spectrumviewer

#endif // REFERENCE_PERIODOGRAM_H_INCLUDED
