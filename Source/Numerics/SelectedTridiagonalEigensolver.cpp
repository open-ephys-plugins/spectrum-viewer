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
#include <cstddef>
#include <cstdint>
#include <iterator>
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

// ---------------------------------------------------------------------------
// In-tree selected symmetric-tridiagonal eigensolver.
//
// Sturm-sequence bisection for the wanted eigenvalues, then inverse iteration
// with a shifted tridiagonal LU for the vectors. Written from the published
// algorithms -- Barth, Martin & Wilkinson (1967) for the bisection; Peters &
// Wilkinson (1979) and Golub & Van Loan sections 8.2 and 8.4 for the inverse
// iteration -- not transliterated from any LAPACK source.
//
// The DPSS application drives two design points. The Slepian tridiagonal
// matrix has absolute eigenvalue gaps of order 1 that do not shrink with N, so
// bisection isolates each wanted eigenvalue comfortably. Its *relative* gaps,
// however, fall to ~1e-9 at N = 60000, far inside LAPACK's 1e-3 cluster
// threshold, so independent inverse iterations would return vectors tens of
// thousands of times less orthogonal than the tests require. All wanted
// vectors are therefore treated as one cluster and reorthogonalized
// unconditionally, which costs O(K^2 N) -- under a millisecond at K <= 8.
// ---------------------------------------------------------------------------

struct MatrixScale
{
    double norm = 0.0;
    double pivotMinimum = 0.0;
    double eigenvalueTolerance = 0.0;
};

MatrixScale computeScale (const std::vector<double>& diagonal,
                          const std::vector<double>& offDiagonal) noexcept
{
    MatrixScale scale;
    const auto order = diagonal.size();

    // Gershgorin infinity norm. Using max|d| + 2max|e| instead would widen the
    // initial bracket and cost bisection steps for no benefit.
    auto largestSquaredOffDiagonal = 0.0;
    for (std::size_t row = 0; row < order; ++row)
    {
        auto radius = std::abs (diagonal[row]);
        if (row > 0)
            radius += std::abs (offDiagonal[row - 1]);
        if (row + 1 < order)
            radius += std::abs (offDiagonal[row]);
        scale.norm = std::max (scale.norm, radius);
    }
    for (const auto value : offDiagonal)
        largestSquaredOffDiagonal = std::max (largestSquaredOffDiagonal, value * value);

    scale.pivotMinimum = std::max (1.0, largestSquaredOffDiagonal)
                         * std::numeric_limits<double>::min();
    scale.eigenvalueTolerance = 2.0 * std::numeric_limits<double>::epsilon()
                                * std::max (scale.norm, 1.0);
    return scale;
}

/** Counts eigenvalues strictly below shift using the Sturm sequence.

    The pivot clamp must be applied before the sign test, and the test must be
    inclusive. Getting either wrong silently miscounts on matrices with an
    exactly zero pivot -- including the 2x2 case the unit tests pin -- and
    bisection then converges to the wrong value. The clamp is also what lets
    this handle zero off-diagonals, exact degeneracy and diagonal matrices
    without any explicit block splitting.
*/
std::size_t countEigenvaluesBelow (const std::vector<double>& diagonal,
                                   const std::vector<double>& squaredOffDiagonal,
                                   double shift,
                                   double pivotMinimum) noexcept
{
    std::size_t count = 0;
    auto pivot = diagonal[0] - shift;
    if (std::abs (pivot) < pivotMinimum)
        pivot = -pivotMinimum;
    if (pivot <= 0.0)
        ++count;

    for (std::size_t row = 1; row < diagonal.size(); ++row)
    {
        pivot = (diagonal[row] - shift) - squaredOffDiagonal[row - 1] / pivot;
        if (std::abs (pivot) < pivotMinimum)
            pivot = -pivotMinimum;
        if (pivot <= 0.0)
            ++count;
    }

    return count;
}

/** Bisects for the eigenvalue at ascending index target. */
double bisectForIndex (const std::vector<double>& diagonal,
                       const std::vector<double>& squaredOffDiagonal,
                       std::size_t target,
                       const MatrixScale& scale) noexcept
{
    auto lower = -scale.norm;
    auto upper = scale.norm;

    // Invariant: count(lower) <= target < count(upper).
    for (int iteration = 0; iteration < 128; ++iteration)
    {
        if (upper - lower
            <= scale.eigenvalueTolerance
                   + 4.0 * std::numeric_limits<double>::epsilon()
                         * std::max (std::abs (lower), std::abs (upper)))
            break;

        const auto middle = lower + (upper - lower) * 0.5;
        if (middle <= lower || middle >= upper)
            break;

        if (countEigenvaluesBelow (diagonal, squaredOffDiagonal, middle,
                                   scale.pivotMinimum)
            <= target)
            lower = middle;
        else
            upper = middle;
    }

    return lower + (upper - lower) * 0.5;
}

/** LU factorization of a shifted tridiagonal matrix, with row interchanges.

    T - shift*I is deliberately close to singular, which is exactly what makes
    inverse iteration converge. A general banded solver refuses such a matrix;
    here a vanishing pivot is clamped instead, because it means the shift has
    landed on an eigenvalue and the amplified solve points where we want it.
*/
struct ShiftedFactorization
{
    std::vector<double> pivot;
    std::vector<double> firstSuperDiagonal;
    std::vector<double> secondSuperDiagonal;
    std::vector<double> multiplier;
    std::vector<unsigned char> swapped;
};

void factorizeShifted (const std::vector<double>& diagonal,
                       const std::vector<double>& offDiagonal,
                       double shift,
                       double pivotFloor,
                       ShiftedFactorization& factorization)
{
    const auto order = diagonal.size();
    auto& pivot = factorization.pivot;
    auto& superOne = factorization.firstSuperDiagonal;
    auto& superTwo = factorization.secondSuperDiagonal;
    auto& multiplier = factorization.multiplier;
    auto& swapped = factorization.swapped;

    pivot.assign (order, 0.0);
    superOne.assign (order == 0 ? 0 : order - 1, 0.0);
    superTwo.assign (order < 2 ? 0 : order - 2, 0.0);
    multiplier.assign (order == 0 ? 0 : order - 1, 0.0);
    swapped.assign (order == 0 ? 0 : order - 1, 0u);

    for (std::size_t row = 0; row < order; ++row)
        pivot[row] = diagonal[row] - shift;
    for (std::size_t row = 0; row + 1 < order; ++row)
        superOne[row] = offDiagonal[row];

    for (std::size_t row = 0; row + 1 < order; ++row)
    {
        const auto subDiagonal = offDiagonal[row];

        if (std::abs (pivot[row]) >= std::abs (subDiagonal))
        {
            swapped[row] = 0u;
            const auto factor = pivot[row] != 0.0 ? subDiagonal / pivot[row] : 0.0;
            multiplier[row] = factor;
            pivot[row + 1] -= factor * superOne[row];
        }
        else
        {
            // Exchange rows, then eliminate. The exchange pushes an element
            // into the second superdiagonal, which is why the solve needs it.
            swapped[row] = 1u;
            const auto factor = pivot[row] / subDiagonal;
            multiplier[row] = factor;

            const auto previousPivotRow = pivot[row];
            const auto previousSuperOne = superOne[row];
            const auto nextPivot = pivot[row + 1];
            const auto nextSuperOne = row + 2 < order ? superOne[row + 1] : 0.0;

            pivot[row] = subDiagonal;
            superOne[row] = nextPivot;
            pivot[row + 1] = previousSuperOne - factor * nextPivot;
            static_cast<void> (previousPivotRow);

            if (row + 2 < order)
            {
                superTwo[row] = nextSuperOne;
                superOne[row + 1] = -factor * nextSuperOne;
            }
        }

        if (std::abs (pivot[row]) <= pivotFloor)
            pivot[row] = pivotFloor;
    }

    if (order > 0 && std::abs (pivot[order - 1]) <= pivotFloor)
        pivot[order - 1] = pivotFloor;
}

void solveShifted (const ShiftedFactorization& factorization,
                   std::vector<double>& vector) noexcept
{
    const auto order = vector.size();
    if (order == 0)
        return;

    for (std::size_t row = 0; row + 1 < order; ++row)
    {
        if (factorization.swapped[row] != 0u)
        {
            const auto lower = vector[row + 1];
            vector[row + 1] = vector[row] - factorization.multiplier[row] * lower;
            vector[row] = lower;
        }
        else
        {
            vector[row + 1] -= factorization.multiplier[row] * vector[row];
        }
    }

    vector[order - 1] /= factorization.pivot[order - 1];
    if (order >= 2)
    {
        const auto row = order - 2;
        vector[row] = (vector[row] - factorization.firstSuperDiagonal[row] * vector[row + 1])
                      / factorization.pivot[row];
    }
    for (std::size_t reverse = 2; reverse < order; ++reverse)
    {
        const auto row = order - 1 - reverse;
        vector[row] = (vector[row]
                       - factorization.firstSuperDiagonal[row] * vector[row + 1]
                       - factorization.secondSuperDiagonal[row] * vector[row + 2])
                      / factorization.pivot[row];
    }
}

/** Deterministic, reentrant start vector.

    No global or function-local static state: the solver is called concurrently
    by the unit tests. std::uniform_real_distribution is avoided because its
    output is implementation-defined, which would make the generated tapers
    differ between libstdc++, libc++ and MSVC.
*/
void fillStartVector (std::vector<double>& vector, std::uint64_t seed) noexcept
{
    auto state = seed * 0x9E3779B97F4A7C15ULL + 0x123456789ABCDEFULL;
    for (auto& value : vector)
    {
        state += 0x9E3779B97F4A7C15ULL;
        auto mixed = state;
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ULL;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBULL;
        mixed ^= mixed >> 31;
        // Map to (-1, 1) by explicit arithmetic on the top 53 bits.
        const auto unitInterval = static_cast<double> (mixed >> 11)
                                  * (1.0 / 9007199254740992.0);
        value = 2.0 * unitInterval - 1.0;
    }
}

double euclideanNorm (const std::vector<double>& vector) noexcept
{
    auto sum = 0.0;
    for (const auto value : vector)
        sum += value * value;
    return std::sqrt (sum);
}

/** Modified Gram-Schmidt against the already-converged vectors. */
void orthogonalizeAgainst (const std::vector<double>& basis,
                           std::size_t order,
                           std::size_t vectorCount,
                           std::vector<double>& vector) noexcept
{
    for (std::size_t index = 0; index < vectorCount; ++index)
    {
        const auto* previous = basis.data() + index * order;
        auto projection = 0.0;
        for (std::size_t row = 0; row < order; ++row)
            projection += previous[row] * vector[row];
        for (std::size_t row = 0; row < order; ++row)
            vector[row] -= projection * previous[row];
    }
}

double normalizedResidual (const std::vector<double>& diagonal,
                           const std::vector<double>& offDiagonal,
                           const std::vector<double>& vector,
                           double eigenvalue,
                           double norm) noexcept
{
    const auto order = diagonal.size();
    auto sum = 0.0;
    for (std::size_t row = 0; row < order; ++row)
    {
        auto value = (diagonal[row] - eigenvalue) * vector[row];
        if (row > 0)
            value += offDiagonal[row - 1] * vector[row - 1];
        if (row + 1 < order)
            value += offDiagonal[row] * vector[row + 1];
        sum += value * value;
    }
    return std::sqrt (sum) / std::max (norm, 1.0);
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

    namespace
    {
        /** Shared argument validation for both backends. */
        bool validateArguments (const std::vector<double>& diagonal,
                                const std::vector<double>& offDiagonal,
                                std::size_t eigenpairCount,
                                SymmetricTridiagonalEigenResult& result) noexcept
        {
            const auto order = diagonal.size();

            if (order == 0
                || order > static_cast<std::size_t> (std::numeric_limits<int>::max()))
            {
                result.status = SymmetricTridiagonalEigenStatus::invalidOrder;
                return false;
            }
            if (eigenpairCount == 0 || eigenpairCount > order
                || eigenpairCount
                       > static_cast<std::size_t> (std::numeric_limits<int>::max()))
            {
                result.status = SymmetricTridiagonalEigenStatus::invalidEigenpairCount;
                return false;
            }
            if (offDiagonal.size() != order - 1)
            {
                result.status = SymmetricTridiagonalEigenStatus::invalidOffDiagonalSize;
                return false;
            }
            if (! isFinite (diagonal) || ! isFinite (offDiagonal))
            {
                result.status = SymmetricTridiagonalEigenStatus::nonFiniteInput;
                return false;
            }
            return true;
        }
    } // namespace

    SymmetricTridiagonalEigenResult findLargestSymmetricTridiagonalEigenpairsInTree (
        const std::vector<double>& diagonal,
        const std::vector<double>& offDiagonal,
        std::size_t eigenpairCount) noexcept
    {
        SymmetricTridiagonalEigenResult result;
        if (! validateArguments (diagonal, offDiagonal, eigenpairCount, result))
            return result;

        const auto order = diagonal.size();

        try
        {
            const auto scale = computeScale (diagonal, offDiagonal);

            std::vector<double> squaredOffDiagonal (order - 1);
            for (std::size_t index = 0; index + 1 < order; ++index)
                squaredOffDiagonal[index] = offDiagonal[index] * offDiagonal[index];

            result.eigenvalues.resize (eigenpairCount);
            result.eigenvectors.assign (order * eigenpairCount, 0.0);

            // Descending order is produced directly: eigenpair k is the
            // ascending index order-1-k. No reversal pass, and therefore no
            // opportunity for an off-by-one in one.
            for (std::size_t pair = 0; pair < eigenpairCount; ++pair)
                result.eigenvalues[pair] = bisectForIndex (
                    diagonal, squaredOffDiagonal, order - 1 - pair, scale);

            ShiftedFactorization factorization;
            std::vector<double> candidate (order);
            std::vector<double> best (order);

            // Deliberately near-unreachable, so the loop below is governed by
            // the stall test rather than by a guessed floor. An earlier version
            // stopped at 8*eps*sqrt(order), which at order 15000 is 2.2e-13 --
            // comfortably above the 1e-13 residual the unit tests require at
            // production scale, so vectors were accepted while still improving.
            const auto achievableResidual =
                4.0 * std::numeric_limits<double>::epsilon();
            const auto pivotFloor = std::numeric_limits<double>::epsilon()
                                    * std::max (scale.norm, 1.0);
            auto unconverged = 0;

            for (std::size_t pair = 0; pair < eigenpairCount; ++pair)
            {
                const auto eigenvalue = result.eigenvalues[pair];
                factorizeShifted (diagonal, offDiagonal, eigenvalue, pivotFloor,
                                  factorization);

                fillStartVector (candidate, static_cast<std::uint64_t> (pair) + 1u);
                auto bestResidual = std::numeric_limits<double>::infinity();
                auto converged = false;

                for (int iteration = 0; iteration < 8; ++iteration)
                {
                    solveShifted (factorization, candidate);

                    // Reorthogonalize inside the loop, not only at the end.
                    // This is what stops the iterate collapsing back onto a
                    // neighbouring eigenvector between solves when the
                    // relative gap is tiny.
                    orthogonalizeAgainst (result.eigenvectors, order, pair, candidate);

                    auto length = euclideanNorm (candidate);
                    if (! (length > 0.0) || ! std::isfinite (length))
                    {
                        // The start vector had no component in the wanted
                        // direction, or the solve overflowed. Restart.
                        fillStartVector (candidate,
                                         static_cast<std::uint64_t> (pair) + 1u
                                             + static_cast<std::uint64_t> (iteration + 1)
                                                   * 8191u);
                        orthogonalizeAgainst (result.eigenvectors, order, pair, candidate);
                        length = euclideanNorm (candidate);
                        if (! (length > 0.0) || ! std::isfinite (length))
                            break;
                    }

                    for (auto& value : candidate)
                        value /= length;

                    const auto currentResidual = normalizedResidual (
                        diagonal, offDiagonal, candidate, eigenvalue, scale.norm);

                    if (currentResidual < bestResidual)
                    {
                        bestResidual = currentResidual;
                        std::copy (candidate.begin(), candidate.end(), best.begin());
                    }

                    if (bestResidual <= achievableResidual)
                    {
                        converged = true;
                        break;
                    }

                    // Keep solving while the residual is still falling
                    // materially. Once it plateaus, the remaining error is in
                    // the shift rather than the vector and further solves only
                    // add rounding.
                    if (iteration > 0 && currentResidual > bestResidual * 0.9)
                    {
                        converged = true;
                        break;
                    }
                }

                if (! converged)
                    ++unconverged;

                std::copy (best.begin(), best.end(),
                           result.eigenvectors.begin()
                               + static_cast<std::ptrdiff_t> (pair * order));
            }

            // Final sweep over the whole set. Without it the mutual
            // orthogonality lands around 1e-13 rather than machine precision.
            for (std::size_t pair = 0; pair < eigenpairCount; ++pair)
            {
                auto* vector = result.eigenvectors.data() + pair * order;
                std::copy_n (vector, order, candidate.begin());
                orthogonalizeAgainst (result.eigenvectors, order, pair, candidate);
                const auto length = euclideanNorm (candidate);
                if (length > 0.0 && std::isfinite (length))
                    for (std::size_t row = 0; row < order; ++row)
                        vector[row] = candidate[row] / length;
            }

            result.order = order;
            result.eigenpairCount = eigenpairCount;
            result.solverInfo = unconverged;
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

    SymmetricTridiagonalEigenResult findLargestSymmetricTridiagonalEigenpairs (
        const std::vector<double>& diagonal,
        const std::vector<double>& offDiagonal,
        std::size_t eigenpairCount) noexcept
    {
        return findLargestSymmetricTridiagonalEigenpairsInTree (
            diagonal, offDiagonal, eigenpairCount);
    }

    SymmetricTridiagonalEigenResult findLargestSymmetricTridiagonalEigenpairsLapack (
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
        return "in-tree symmetric tridiagonal bisection and inverse iteration, "
               "double precision, one thread";
    }
} // namespace numerics
} // namespace spectrumviewer
