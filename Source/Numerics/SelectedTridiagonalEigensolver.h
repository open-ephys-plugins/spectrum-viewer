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

    A success status means the solve ran to completion, not that every vector is
    usable. Callers must also require solverInfo == 0.
*/
    struct SymmetricTridiagonalEigenResult
    {
        SymmetricTridiagonalEigenStatus status = SymmetricTridiagonalEigenStatus::solverFailure;

        /** Eigenpairs whose normalized residual stalled above the solver's
            acceptance bar of 1e-13. Nonzero means those vectors are not usable
            even though status is success. */
        int solverInfo = 0;

        /** Largest normalized residual over the returned eigenpairs, and the
            quantity solverInfo counts against. */
        double largestResidual = 0.0;

        /** Inverse-iteration solves performed in total, across all eigenpairs.
            Two per pair means every pair stopped at its first refinement. */
        int refinementSolveCount = 0;

        std::size_t order = 0;
        std::size_t eigenpairCount = 0;
        std::vector<double> eigenvalues;
        std::vector<double> eigenvectors;

        bool succeeded() const noexcept;
        const double* getEigenvector (std::size_t eigenpairIndex) const noexcept;
    };

    /** Finds the algebraically largest eigenpairs of a real tridiagonal matrix.

    This function allocates and runs an iterative eigensolver, so callers must
    run it outside real-time callbacks.
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
