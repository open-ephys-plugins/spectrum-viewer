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

#ifndef SPECTRUM_ESTIMATION_H_INCLUDED
#define SPECTRUM_ESTIMATION_H_INCLUDED

#include <cstddef>
#include <cstdint>

namespace spectrumviewer
{
enum class DetrendMode
{
    none,
    mean,
    linear
};

enum class SpectrumValueKind
{
    powerSpectralDensity
};

/** Immutable DSP metadata shared by every frame from one analysis configuration. */
struct SpectrumFrameDescriptor
{
    double sampleRateHz = 0.0;
    double binWidthHz = 0.0;
    double timeHalfBandwidth = 0.0;
    std::size_t windowSampleCount = 0;
    std::size_t hopSampleCount = 0;
    std::size_t taperCount = 0;
    std::uint64_t configurationGeneration = 0;
    DetrendMode detrendMode = DetrendMode::mean;
    SpectrumValueKind valueKind = SpectrumValueKind::powerSpectralDensity;
};

/** One chronological channel represented by one or two contiguous regions. */
struct ChannelSampleView
{
    const float* firstData = nullptr;
    std::size_t firstSize = 0;
    const float* secondData = nullptr;
    std::size_t secondSize = 0;
};
} // namespace spectrumviewer

#endif // SPECTRUM_ESTIMATION_H_INCLUDED
