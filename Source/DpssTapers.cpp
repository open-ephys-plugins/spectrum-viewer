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

#include "DpssTapers.h"

#include <OpenEphysFFTWBatch.h>
#include <OpenEphysNumerics.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <vector>

namespace spectrumviewer
{
namespace
{
    constexpr double pi = 3.141592653589793238462643383279502884;

    int getAutocorrelationLength (std::size_t sampleCount)
    {
        if (sampleCount > (static_cast<std::size_t> (std::numeric_limits<int>::max()) + 1U) / 2U)
            return 0;

        const auto minimumLength = 2U * sampleCount - 1U;
        std::size_t length = 1;
        while (length < minimumLength)
            length *= 2U;

        return length <= static_cast<std::size_t> (std::numeric_limits<int>::max())
                   ? static_cast<int> (length)
                   : 0;
    }

    void canonicalizeSigns (std::vector<double>& tapers,
                            std::size_t sampleCount,
                            std::size_t taperCount)
    {
        const auto oddThreshold = std::max (1.0e-7, 1.0 / static_cast<double> (sampleCount));
        for (std::size_t taper = 0; taper < taperCount; ++taper)
        {
            auto* values = tapers.data() + taper * sampleCount;
            bool reverse = false;
            if (taper % 2U == 0U)
            {
                const auto sum = std::accumulate (values, values + sampleCount, 0.0);
                reverse = sum < 0.0;
            }
            else
            {
                const auto firstLobe = std::find_if (values, values + sampleCount, [oddThreshold] (double value)
                                                     { return value * value > oddThreshold; });
                reverse = firstLobe != values + sampleCount && *firstLobe < 0.0;
            }

            if (reverse)
                std::transform (values, values + sampleCount, values, [] (double value)
                                { return -value; });
        }
    }

    bool calculateConcentrationRatios (const std::vector<double>& tapers,
                                       std::size_t sampleCount,
                                       std::size_t taperCount,
                                       double timeHalfBandwidth,
                                       unsigned int planningFlags,
                                       std::vector<double>& ratios)
    {
        const auto transformLength = getAutocorrelationLength (sampleCount);
        if (transformLength == 0
            || taperCount > static_cast<std::size_t> (std::numeric_limits<int>::max()))
            return false;

        FFTWRealToComplexBatchDouble forward (transformLength,
                                              static_cast<int> (taperCount),
                                              planningFlags);
        FFTWComplexToRealBatchDouble inverse (transformLength,
                                              static_cast<int> (taperCount),
                                              planningFlags);

        for (std::size_t taper = 0; taper < taperCount; ++taper)
        {
            auto* input = forward.getInputPointer (static_cast<int> (taper));
            std::copy_n (tapers.data() + taper * sampleCount, sampleCount, input);
            std::fill (input + sampleCount, input + transformLength, 0.0);
        }
        forward.execute();

        for (std::size_t taper = 0; taper < taperCount; ++taper)
        {
            const auto* spectrum = forward.getOutputPointer (static_cast<int> (taper));
            auto* power = inverse.getInputPointer (static_cast<int> (taper));
            for (int bin = 0; bin < forward.getBinCount(); ++bin)
                power[bin] = { std::norm (spectrum[bin]), 0.0 };
        }
        inverse.execute();

        const auto halfBandwidth = timeHalfBandwidth / static_cast<double> (sampleCount);
        const auto inverseLength = 1.0 / static_cast<double> (transformLength);
        ratios.resize (taperCount);
        for (std::size_t taper = 0; taper < taperCount; ++taper)
        {
            const auto* autocorrelation = inverse.getOutputPointer (static_cast<int> (taper));
            auto ratio = 2.0 * halfBandwidth * autocorrelation[0] * inverseLength;
            for (std::size_t lag = 1; lag < sampleCount; ++lag)
            {
                const auto argument = 2.0 * halfBandwidth * static_cast<double> (lag);
                const auto sinc = std::sin (pi * argument) / (pi * argument);
                ratio += 4.0 * halfBandwidth * sinc * autocorrelation[lag] * inverseLength;
            }

            if (! std::isfinite (ratio) || ratio < -1.0e-12 || ratio > 1.0 + 1.0e-12)
                return false;
            ratios[taper] = std::clamp (ratio, 0.0, 1.0);
        }
        return true;
    }
} // namespace

bool DpssTaperBank::succeeded() const noexcept
{
    return status == DpssGenerationStatus::success;
}

const float* DpssTaperBank::getTaper (std::size_t taperIndex) const noexcept
{
    if (taperIndex >= taperCount || sampleCount == 0)
        return nullptr;
    return tapers.data() + taperIndex * sampleCount;
}

DpssTaperBank generateDpssTapers (std::size_t sampleCount,
                                  double timeHalfBandwidth,
                                  std::size_t taperCount,
                                  unsigned int fftPlanningFlags) noexcept
{
    DpssTaperBank bank;
    if (sampleCount < 2 || getAutocorrelationLength (sampleCount) == 0)
    {
        bank.status = DpssGenerationStatus::invalidSampleCount;
        return bank;
    }
    if (! std::isfinite (timeHalfBandwidth) || timeHalfBandwidth <= 0.0
        || timeHalfBandwidth >= 0.5 * static_cast<double> (sampleCount))
    {
        bank.status = DpssGenerationStatus::invalidTimeHalfBandwidth;
        return bank;
    }
    if (taperCount == 0 || taperCount > sampleCount)
    {
        bank.status = DpssGenerationStatus::invalidTaperCount;
        return bank;
    }

    try
    {
        std::vector<double> diagonal (sampleCount);
        std::vector<double> offDiagonal (sampleCount - 1);
        const auto modulation = std::cos (2.0 * pi * timeHalfBandwidth
                                          / static_cast<double> (sampleCount));
        for (std::size_t sample = 0; sample < sampleCount; ++sample)
        {
            const auto centred = (static_cast<double> (sampleCount - 1)
                                  - 2.0 * static_cast<double> (sample))
                                 / 2.0;
            diagonal[sample] = centred * centred * modulation;
        }
        for (std::size_t sample = 0; sample + 1 < sampleCount; ++sample)
        {
            const auto oneBased = static_cast<double> (sample + 1);
            offDiagonal[sample] = oneBased
                                  * (static_cast<double> (sampleCount) - oneBased) / 2.0;
        }

        auto eigenpairs = OpenEphys::Numerics::findLargestSymmetricTridiagonalEigenpairs (
            diagonal, offDiagonal, taperCount);
        if (! eigenpairs.succeeded())
        {
            bank.status = DpssGenerationStatus::eigenSolverFailure;
            bank.eigenSolverInfo = eigenpairs.solverInfo;
            return bank;
        }

        canonicalizeSigns (eigenpairs.eigenvectors, sampleCount, taperCount);
        if (! calculateConcentrationRatios (eigenpairs.eigenvectors,
                                            sampleCount,
                                            taperCount,
                                            timeHalfBandwidth,
                                            fftPlanningFlags,
                                            bank.concentrationRatios))
        {
            bank.status = DpssGenerationStatus::concentrationFailure;
            return bank;
        }

        bank.sampleCount = sampleCount;
        bank.taperCount = taperCount;
        bank.timeHalfBandwidth = timeHalfBandwidth;
        bank.tapers.resize (eigenpairs.eigenvectors.size());
        std::transform (eigenpairs.eigenvectors.begin(),
                        eigenpairs.eigenvectors.end(),
                        bank.tapers.begin(),
                        [] (double value)
                        { return static_cast<float> (value); });
        bank.status = DpssGenerationStatus::success;
        return bank;
    }
    catch (const std::bad_alloc&)
    {
        bank.status = DpssGenerationStatus::allocationFailure;
        return bank;
    }
    catch (...)
    {
        bank.status = DpssGenerationStatus::concentrationFailure;
        return bank;
    }
}

const char* getDpssGenerationStatusDescription (DpssGenerationStatus status) noexcept
{
    switch (status)
    {
        case DpssGenerationStatus::success:
            return "success";
        case DpssGenerationStatus::invalidSampleCount:
            return "invalid sample count";
        case DpssGenerationStatus::invalidTimeHalfBandwidth:
            return "invalid time-half-bandwidth product";
        case DpssGenerationStatus::invalidTaperCount:
            return "invalid taper count";
        case DpssGenerationStatus::eigenSolverFailure:
            return "DPSS eigensolver failure";
        case DpssGenerationStatus::concentrationFailure:
            return "concentration-ratio calculation failure";
        case DpssGenerationStatus::allocationFailure:
            return "allocation failure";
    }
    return "unknown error";
}
} // namespace spectrumviewer
