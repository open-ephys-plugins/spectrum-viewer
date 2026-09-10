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

#include "SelectedTridiagonalEigensolver.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

#ifndef SPECTRUM_VIEWER_OPENBLAS_VERSION
#error "The Spectrum Viewer OpenBLAS target must define its backend version"
#endif

namespace
{
constexpr int lapackColumnMajor = 102;

extern "C" int LAPACKE_dstevr (int matrixLayout,
                               char jobz,
                               char range,
                               int n,
                               double* diagonal,
                               double* offDiagonal,
                               double valueLowerBound,
                               double valueUpperBound,
                               int indexLowerBound,
                               int indexUpperBound,
                               double absoluteTolerance,
                               int* eigenvalueCount,
                               double* eigenvalues,
                               double* eigenvectors,
                               int leadingDimension,
                               int* support);
extern "C" void openblas_set_num_threads (int threadCount);

void configureOpenBlas() noexcept
{
    static const auto configured = []
    {
        openblas_set_num_threads (1);
        return true;
    }();
    static_cast<void> (configured);
}

bool isFinite (const std::vector<double>& values) noexcept
{
    return std::all_of (values.begin(), values.end(), [] (double value)
                        { return std::isfinite (value); });
}
} // namespace

namespace spectrumviewer
{
namespace numerics
{
    bool SymmetricTridiagonalEigenResult::succeeded() const noexcept
    {
        return status == SymmetricTridiagonalEigenStatus::success;
    }

    const double* SymmetricTridiagonalEigenResult::getEigenvector (
        std::size_t eigenpairIndex) const noexcept
    {
        if (eigenpairIndex >= eigenpairCount || order == 0)
            return nullptr;

        return eigenvectors.data() + eigenpairIndex * order;
    }

    SymmetricTridiagonalEigenResult findLargestSymmetricTridiagonalEigenpairs (
        const std::vector<double>& diagonal,
        const std::vector<double>& offDiagonal,
        std::size_t eigenpairCount) noexcept
    {
        SymmetricTridiagonalEigenResult result;
        const auto order = diagonal.size();

        if (order == 0
            || order > static_cast<std::size_t> (std::numeric_limits<int>::max()))
        {
            result.status = SymmetricTridiagonalEigenStatus::invalidOrder;
            return result;
        }
        if (eigenpairCount == 0 || eigenpairCount > order
            || eigenpairCount > static_cast<std::size_t> (std::numeric_limits<int>::max()))
        {
            result.status = SymmetricTridiagonalEigenStatus::invalidEigenpairCount;
            return result;
        }
        if (offDiagonal.size() != order - 1)
        {
            result.status = SymmetricTridiagonalEigenStatus::invalidOffDiagonalSize;
            return result;
        }
        if (! isFinite (diagonal) || ! isFinite (offDiagonal))
        {
            result.status = SymmetricTridiagonalEigenStatus::nonFiniteInput;
            return result;
        }

        try
        {
            configureOpenBlas();

            auto mutableDiagonal = diagonal;
            auto mutableOffDiagonal = offDiagonal;
            mutableOffDiagonal.resize (std::max<std::size_t> (1, order - 1));

            const auto lapackOrder = static_cast<int> (order);
            const auto requestedCount = static_cast<int> (eigenpairCount);
            std::vector<double> ascendingEigenvalues (eigenpairCount);
            std::vector<double> columnMajorEigenvectors (order * eigenpairCount);
            std::vector<int> support (2 * eigenpairCount);
            int returnedCount = 0;

            result.solverInfo = LAPACKE_dstevr (lapackColumnMajor,
                                                'V',
                                                'I',
                                                lapackOrder,
                                                mutableDiagonal.data(),
                                                mutableOffDiagonal.data(),
                                                0.0,
                                                0.0,
                                                lapackOrder - requestedCount + 1,
                                                lapackOrder,
                                                0.0,
                                                &returnedCount,
                                                ascendingEigenvalues.data(),
                                                columnMajorEigenvectors.data(),
                                                lapackOrder,
                                                support.data());

            if (result.solverInfo != 0 || returnedCount != requestedCount)
            {
                result.status = SymmetricTridiagonalEigenStatus::solverFailure;
                return result;
            }

            result.order = order;
            result.eigenpairCount = eigenpairCount;
            result.eigenvalues.resize (eigenpairCount);
            result.eigenvectors.resize (order * eigenpairCount);

            for (std::size_t destination = 0; destination < eigenpairCount; ++destination)
            {
                const auto source = eigenpairCount - 1 - destination;
                result.eigenvalues[destination] = ascendingEigenvalues[source];
                std::copy_n (columnMajorEigenvectors.data() + source * order,
                             order,
                             result.eigenvectors.data() + destination * order);
            }

            result.status = SymmetricTridiagonalEigenStatus::success;
            return result;
        }
        catch (const std::bad_alloc&)
        {
            result.status = SymmetricTridiagonalEigenStatus::allocationFailure;
            return result;
        }
        catch (...)
        {
            result.status = SymmetricTridiagonalEigenStatus::solverFailure;
            return result;
        }
    }

    const char* getSymmetricTridiagonalEigenStatusDescription (
        SymmetricTridiagonalEigenStatus status) noexcept
    {
        switch (status)
        {
            case SymmetricTridiagonalEigenStatus::success:
                return "success";
            case SymmetricTridiagonalEigenStatus::invalidOrder:
                return "invalid matrix order";
            case SymmetricTridiagonalEigenStatus::invalidEigenpairCount:
                return "invalid eigenpair count";
            case SymmetricTridiagonalEigenStatus::invalidOffDiagonalSize:
                return "invalid off-diagonal size";
            case SymmetricTridiagonalEigenStatus::nonFiniteInput:
                return "non-finite matrix element";
            case SymmetricTridiagonalEigenStatus::allocationFailure:
                return "allocation failure";
            case SymmetricTridiagonalEigenStatus::solverFailure:
                return "eigensolver failure";
        }

        return "unknown error";
    }

    const char* getNumericsBackendDescription() noexcept
    {
        return "conda-forge OpenBLAS " SPECTRUM_VIEWER_OPENBLAS_VERSION
               ", LP64, one thread";
    }
} // namespace numerics
} // namespace spectrumviewer
