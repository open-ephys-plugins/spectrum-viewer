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

#ifndef SPECTRUM_REFERENCE_FILE_H_INCLUDED
#define SPECTRUM_REFERENCE_FILE_H_INCLUDED

#include "SpectrumReference.h"

#include <AppConfig.h>
#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace spectrumviewer
{
/** Version written to, and the newest accepted from, a reference file. */
constexpr int spectrumReferenceFileVersion = 1;

/** Serializes one channel of a captured spectrum as a baseline reference file,
    so a baseline captured under ideal conditions in one session can be
    compared against in another.

    The file is JSON. Metadata reads as written. The mean PSD and the sample
    variance are base64-encoded little-endian float32, one value per bin, so
    each loads directly with numpy.frombuffer (base64.b64decode (s), "<f4").

    The doubles that compatibility compares exactly are also written as their
    IEEE-754 bit patterns, and those are what is read back. A decimal round
    trip is not guaranteed to be exact, and a single ulp of difference would
    make the baseline incompatible with the configuration that produced it.

    channel is a position within the spectrum's channels, not a channel index;
    std::out_of_range is thrown if there is no such channel. labels supplies
    the stream and channel names to record. They are informational only; pass
    the capture's own labels when it has them. */
juce::String writeSpectrumReferenceFile (const CapturedSpectrum& spectrum,
                                         std::size_t channel,
                                         const SpectrumSourceLabels& labels);

/** Parses a reference file into a baseline (see CapturedSpectrum). The result
    carries captureId, which identifies it within this session.

    Returns nullptr and sets error to a sentence suitable for the user if the
    text is not a valid reference file. */
std::shared_ptr<const CapturedSpectrum> readSpectrumReferenceFile (const juce::String& text,
                                                                   std::uint64_t captureId,
                                                                   juce::String& error);
} // namespace spectrumviewer

#endif // SPECTRUM_REFERENCE_FILE_H_INCLUDED
