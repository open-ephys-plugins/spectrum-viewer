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

#ifndef SELECTED_TRIDIAGONAL_EIGENSOLVER_H_INCLUDED
#define SELECTED_TRIDIAGONAL_EIGENSOLVER_H_INCLUDED

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
namespace numerics
{
    enum class SymmetricTridiagonalEigenStatus
    {
        success,
        invalidOrder,
        invalidEigenpairCount,
        invalidOffDiagonalSize,
        nonFiniteInput,
        allocationFailure,
        solverFailure
    };

    /** Result of a selected symmetric-tridiagonal eigensolve.

    Eigenvalues are in descending order. Eigenvectors are eigenpair-major, with
    each vector stored as order contiguous doubles.
*/
    struct SymmetricTridiagonalEigenResult
    {
        SymmetricTridiagonalEigenStatus status = SymmetricTridiagonalEigenStatus::solverFailure;
        int solverInfo = 0;
        std::size_t order = 0;
        std::size_t eigenpairCount = 0;
        std::vector<double> eigenvalues;
        std::vector<double> eigenvectors;

        bool succeeded() const noexcept;
        const double* getEigenvector (std::size_t eigenpairIndex) const noexcept;
    };

    /** Finds the algebraically largest eigenpairs of a real tridiagonal matrix.

    This function allocates and invokes LAPACK, so callers must run it outside
    real-time callbacks.
*/
    SymmetricTridiagonalEigenResult
        findLargestSymmetricTridiagonalEigenpairs (const std::vector<double>& diagonal,
                                                   const std::vector<double>& offDiagonal,
                                                   std::size_t eigenpairCount) noexcept;

    const char* getSymmetricTridiagonalEigenStatusDescription (
        SymmetricTridiagonalEigenStatus status) noexcept;

    const char* getNumericsBackendDescription() noexcept;
} // namespace numerics
} // namespace spectrumviewer

#endif // SELECTED_TRIDIAGONAL_EIGENSOLVER_H_INCLUDED
