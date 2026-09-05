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

#ifndef CUMULATIVE_TFR_H_INCLUDED
#define CUMULATIVE_TFR_H_INCLUDED

#include <OpenEphysFFTW.h>

#include <vector>

using FFTWArrayType = FFTWTransformableArrayUsing<0U>;

/** Computes power spectra from pre-windowed real-valued FFT buffers. */
class CumulativeTFR
{
public:
    CumulativeTFR (int numChannels, int numFrequencies, float frequencyStep, int frequencyStart);

    /** Transforms one channel and stores its current power spectrum. */
    void computeFFT (FFTWArrayType& fftBuffer, int channelIndex);

    /** Copies the current power spectrum for one channel into preallocated storage. */
    void getPower (std::vector<float>& destination, int channelIndex) const;

private:
    const int nFreqs;
    const int firstBin;
    std::vector<std::vector<float>> powerByChannel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CumulativeTFR);
};

#endif // CUMULATIVE_TFR_H_INCLUDED