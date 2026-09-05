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
#include "CumulativeTFR.h"

#include <algorithm>
#include <complex>

CumulativeTFR::CumulativeTFR (int numChannels, int numFrequencies, float frequencyStep, int frequencyStart)
        : nFreqs (jmax (1, numFrequencies)),
            firstBin (roundToInt (frequencyStart / jmax (frequencyStep, 1.0e-6f))),
            powerByChannel ((size_t) jmax (1, numChannels), std::vector<float> ((size_t) nFreqs, 0.0f))
{
    // Reconfiguration validates these values; assertions keep that contract
    // visible if this helper is reused from another call site.
    jassert (numChannels > 0);
    jassert (numFrequencies > 0);
    jassert (frequencyStep > 0.0f);
}

void CumulativeTFR::computeFFT (FFTWArrayType& fftBuffer, int channelIndex)
{
    if (! isPositiveAndBelow (channelIndex, (int) powerByChannel.size()))
    {
        jassertfalse;
        return;
    }

    // fftReal() stores the non-negative frequency bins in the first N/2 + 1
    // complex entries of the in-place transform buffer.
    fftBuffer.fftReal();
    auto& channelPower = powerByChannel[(size_t) channelIndex];
    const int availableBins = jmax (0, fftBuffer.getLength() / 2 + 1 - firstBin);
    const int binsToCopy = jmin (nFreqs, availableBins);
    jassert (binsToCopy == nFreqs);

    // std::norm avoids the square root performed by abs() followed by squaring.
    for (int freq = 0; freq < binsToCopy; ++freq)
        channelPower[(size_t) freq] = (float) std::norm (fftBuffer.getAsComplex (firstBin + freq));

    std::fill (channelPower.begin() + binsToCopy, channelPower.end(), 0.0f);
}

void CumulativeTFR::getPower (std::vector<float>& destination, int channelIndex) const
{
    if (! isPositiveAndBelow (channelIndex, (int) powerByChannel.size()))
    {
        jassertfalse;
        return;
    }

    // Destination storage belongs to an AtomicallyShared slot and is allocated
    // during reconfiguration, so this copy performs no worker-thread allocation.
    const auto& source = powerByChannel[(size_t) channelIndex];
    const auto valuesToCopy = jmin (destination.size(), source.size());
    std::copy_n (source.begin(), valuesToCopy, destination.begin());

    if (valuesToCopy < destination.size())
        std::fill (destination.begin() + valuesToCopy, destination.end(), 0.0f);
}
