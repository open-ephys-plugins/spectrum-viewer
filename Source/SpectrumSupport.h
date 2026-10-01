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

#ifndef SPECTRUM_SUPPORT_H_INCLUDED
#define SPECTRUM_SUPPORT_H_INCLUDED

#include "SpectrumEstimation.h"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <stdexcept>

/**
    Small helpers shared by the spectral estimators and the transport buffers.

    Internal to the plugin. Each helper exists once here because its copies had
    started to drift; keep call-site policy, such as whether an empty dimension
    is an error, at the call site.
*/
namespace spectrumviewer::support
{
/** Multiplies sizes, returning false rather than wrapping when the product
    does not fit in std::size_t. A zero factor gives a product of zero. */
inline bool tryMultiply (std::initializer_list<std::size_t> factors,
                         std::size_t& product) noexcept
{
    product = 1;
    for (const auto factor : factors)
    {
        if (factor != 0 && product > std::numeric_limits<std::size_t>::max() / factor)
            return false;
        product *= factor;
    }
    return true;
}

/** The product of sizes, throwing Error with message when it does not fit.
    A zero factor gives zero; a caller for which that is invalid checks first,
    or uses checkedPositiveProduct(). */
template <typename Error = std::length_error>
std::size_t checkedProduct (std::initializer_list<std::size_t> factors, const char* message)
{
    std::size_t product = 0;
    if (! tryMultiply (factors, product))
        throw Error (message);
    return product;
}

/** As checkedProduct(), except that a zero factor is invalid too. Both failures
    throw std::invalid_argument. */
inline std::size_t checkedPositiveProduct (std::initializer_list<std::size_t> factors,
                                           const char* message)
{
    for (const auto factor : factors)
        if (factor == 0)
            throw std::invalid_argument (message);
    return checkedProduct<std::invalid_argument> (factors, message);
}

/** A positive size as the int that FFTW and JUCE take. Throws
    std::invalid_argument if it is zero or does not fit. */
inline int checkedPositiveInt (std::size_t value, const char* message)
{
    if (value == 0 || value > static_cast<std::size_t> (std::numeric_limits<int>::max()))
        throw std::invalid_argument (message);
    return static_cast<int> (value);
}

/** The juce::AbstractFifo size for a FIFO of capacity slots. AbstractFifo keeps
    one slot empty to tell full from empty, so this is one more than capacity.
    Throws std::invalid_argument if capacity is zero or the size does not fit. */
inline int checkedFifoSize (std::size_t capacity, const char* message)
{
    if (capacity == 0 || capacity >= static_cast<std::size_t> (std::numeric_limits<int>::max()))
        throw std::invalid_argument (message);
    return static_cast<int> (capacity + 1);
}

constexpr bool isValidDetrendMode (DetrendMode mode) noexcept
{
    return mode == DetrendMode::none || mode == DetrendMode::mean || mode == DetrendMode::linear;
}

/** A fitted trend, evaluated at sample index i as
    intercept + slope * (i - (N - 1) / 2). */
struct LinearTrend
{
    double intercept = 0.0;
    double slope = 0.0;
};

/** Whether a bin of a one-sided spectrum also carries its negative-frequency
    mirror, so that its power is doubled. DC has no mirror, and neither does
    Nyquist, which exists only for an even sample count.

    ReferencePeriodogram deliberately keeps its own copy of this rule: it is
    the oracle the estimators are checked against, so it must not share the
    code it is checking. */
constexpr bool isFoldedOneSidedBin (std::size_t bin, std::size_t sampleCount) noexcept
{
    const auto isNyquist = sampleCount % 2 == 0 && bin == sampleCount / 2;
    return bin != 0 && ! isNyquist;
}
} // namespace spectrumviewer::support

#endif // SPECTRUM_SUPPORT_H_INCLUDED
