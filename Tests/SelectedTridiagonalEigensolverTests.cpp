#include "Numerics/SelectedTridiagonalEigensolver.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <limits>
#include <numeric>
#include <vector>

namespace
{
double dot (const double* left, const double* right, std::size_t count)
{
    double value = 0.0;
    for (std::size_t index = 0; index < count; ++index)
        value += left[index] * right[index];
    return value;
}

double normalizedResidual (const std::vector<double>& diagonal,
                           const std::vector<double>& offDiagonal,
                           const double* eigenvector,
                           double eigenvalue)
{
    double squaredResidual = 0.0;
    double squaredVector = 0.0;
    double matrixScale = 0.0;

    for (std::size_t index = 0; index < diagonal.size(); ++index)
    {
        auto transformed = diagonal[index] * eigenvector[index];
        if (index > 0)
            transformed += offDiagonal[index - 1] * eigenvector[index - 1];
        if (index + 1 < diagonal.size())
            transformed += offDiagonal[index] * eigenvector[index + 1];

        const auto residual = transformed - eigenvalue * eigenvector[index];
        squaredResidual += residual * residual;
        squaredVector += eigenvector[index] * eigenvector[index];
        matrixScale = std::max (matrixScale, std::abs (diagonal[index]));
        if (index < offDiagonal.size())
            matrixScale = std::max (matrixScale, std::abs (offDiagonal[index]));
    }

    return std::sqrt (squaredResidual)
           / ((matrixScale + std::abs (eigenvalue)) * std::sqrt (squaredVector));
}

void expectValidEigenpairs (const std::vector<double>& diagonal,
                            const std::vector<double>& offDiagonal,
                            std::size_t count,
                            double residualTolerance,
                            double orthogonalityTolerance)
{
    const auto result = spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
        diagonal, offDiagonal, count);
    ASSERT_TRUE (result.succeeded())
        << spectrumviewer::numerics::getSymmetricTridiagonalEigenStatusDescription (
               result.status)
        << ", solver info " << result.solverInfo;
    // A success status only means the solve ran. This is the assertion that
    // catches inverse iteration stopping before the vectors are usable.
    ASSERT_EQ (result.solverInfo, 0)
        << "largest residual " << result.largestResidual;
    // Solves per eigenpair are platform-dependent - the residual plateaus after
    // two on some targets and later on others - so only the bounds are pinned.
    EXPECT_GE (result.refinementSolveCount, static_cast<int> (count));
    EXPECT_LE (result.refinementSolveCount, 16 * static_cast<int> (count));
    ASSERT_EQ (result.order, diagonal.size());
    ASSERT_EQ (result.eigenpairCount, count);
    ASSERT_EQ (result.eigenvalues.size(), count);
    ASSERT_EQ (result.eigenvectors.size(), diagonal.size() * count);

    for (std::size_t eigenpair = 0; eigenpair < count; ++eigenpair)
    {
        if (eigenpair > 0)
        {
            EXPECT_GT (result.eigenvalues[eigenpair - 1], result.eigenvalues[eigenpair]);
        }

        const auto* vector = result.getEigenvector (eigenpair);
        ASSERT_NE (vector, nullptr);
        EXPECT_LT (normalizedResidual (diagonal,
                                       offDiagonal,
                                       vector,
                                       result.eigenvalues[eigenpair]),
                   residualTolerance);
        EXPECT_NEAR (dot (vector, vector, diagonal.size()), 1.0, orthogonalityTolerance);

        for (std::size_t other = 0; other < eigenpair; ++other)
            EXPECT_NEAR (dot (vector, result.getEigenvector (other), diagonal.size()),
                         0.0,
                         orthogonalityTolerance);
    }
}
} // namespace

TEST (SelectedTridiagonalEigensolverTests, SolvesKnownTwoByTwoMatrix)
{
    const auto result = spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
        { 2.0, 2.0 }, { 1.0 }, 2);

    ASSERT_TRUE (result.succeeded());
    ASSERT_EQ (result.eigenvalues.size(), 2U);
    EXPECT_NEAR (result.eigenvalues[0], 3.0, 1.0e-14);
    EXPECT_NEAR (result.eigenvalues[1], 1.0, 1.0e-14);
    EXPECT_NEAR (std::abs (result.getEigenvector (0)[0]), std::sqrt (0.5), 1.0e-14);
    EXPECT_NEAR (std::abs (result.getEigenvector (1)[0]), std::sqrt (0.5), 1.0e-14);
    EXPECT_EQ (result.getEigenvector (2), nullptr);
}

// Pins the backend identity. This is the tripwire that fires if an external
// numerical library is ever linked back in.
TEST (SelectedTridiagonalEigensolverTests, ReportsTheInTreeBackend)
{
    EXPECT_STREQ (spectrumviewer::numerics::getNumericsBackendDescription(),
                  "in-tree symmetric tridiagonal bisection and inverse iteration, "
                  "double precision, one thread");
}

TEST (SelectedTridiagonalEigensolverTests, RejectsInvalidInputs)
{
    using spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs;
    using spectrumviewer::numerics::SymmetricTridiagonalEigenStatus;

    EXPECT_EQ (findLargestSymmetricTridiagonalEigenpairs ({}, {}, 1).status,
               SymmetricTridiagonalEigenStatus::invalidOrder);
    EXPECT_EQ (findLargestSymmetricTridiagonalEigenpairs ({ 1.0 }, {}, 0).status,
               SymmetricTridiagonalEigenStatus::invalidEigenpairCount);
    EXPECT_EQ (findLargestSymmetricTridiagonalEigenpairs ({ 1.0 }, {}, 2).status,
               SymmetricTridiagonalEigenStatus::invalidEigenpairCount);
    EXPECT_EQ (findLargestSymmetricTridiagonalEigenpairs ({ 1.0, 2.0 }, {}, 1).status,
               SymmetricTridiagonalEigenStatus::invalidOffDiagonalSize);
    EXPECT_EQ (findLargestSymmetricTridiagonalEigenpairs (
                   { 1.0, std::numeric_limits<double>::infinity() }, { 0.0 }, 1)
                   .status,
               SymmetricTridiagonalEigenStatus::nonFiniteInput);
}

TEST (SelectedTridiagonalEigensolverTests, ProducesAccurateSelectedEigenpairs)
{
    constexpr std::size_t order = 128;
    std::vector<double> diagonal (order);
    std::vector<double> offDiagonal (order - 1);
    for (std::size_t index = 0; index < order; ++index)
        diagonal[index] = 2.0 + 0.01 * static_cast<double> (index);
    for (std::size_t index = 0; index + 1 < order; ++index)
        offDiagonal[index] = -1.0 + 0.001 * static_cast<double> (index);

    expectValidEigenpairs (diagonal, offDiagonal, 7, 2.0e-14, 2.0e-13);
}

TEST (SelectedTridiagonalEigensolverTests, HandlesProductionScaleDpssMatrix)
{
    constexpr std::size_t order = 60000;
    constexpr std::size_t count = 5;
    constexpr double timeHalfBandwidth = 3.0;
    constexpr double pi = 3.141592653589793238462643383279502884;

    std::vector<double> diagonal (order);
    std::vector<double> offDiagonal (order - 1);
    const auto modulation = std::cos (2.0 * pi * timeHalfBandwidth
                                      / static_cast<double> (order));
    for (std::size_t index = 0; index < order; ++index)
    {
        const auto centered = (static_cast<double> (order - 1)
                               - 2.0 * static_cast<double> (index))
                              / 2.0;
        diagonal[index] = centered * centered * modulation;
    }
    for (std::size_t index = 0; index + 1 < order; ++index)
    {
        const auto oneBased = static_cast<double> (index + 1);
        offDiagonal[index] = oneBased * (static_cast<double> (order) - oneBased) / 2.0;
    }

    expectValidEigenpairs (diagonal, offDiagonal, count, 1.0e-13, 5.0e-12);
}

TEST (SelectedTridiagonalEigensolverTests, SupportsConcurrentIndependentCalls)
{
    constexpr std::size_t taskCount = 4;
    std::vector<std::future<spectrumviewer::numerics::SymmetricTridiagonalEigenResult>> tasks;
    tasks.reserve (taskCount);

    for (std::size_t task = 0; task < taskCount; ++task)
    {
        tasks.emplace_back (std::async (std::launch::async, [task]
                                        {
            constexpr std::size_t order = 512;
            std::vector<double> diagonal (
                order, 2.0 + 0.1 * static_cast<double> (task));
            std::vector<double> offDiagonal (order - 1, -1.0);
            return spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
                diagonal, offDiagonal, 5); }));
    }

    for (auto& task : tasks)
    {
        const auto result = task.get();
        ASSERT_TRUE (result.succeeded());
        ASSERT_EQ (result.eigenpairCount, 5U);
    }
}

// The four cases below exercise the pivot clamps in the Sturm recurrence and
// in the shifted factorization. Without them the in-tree solver either divides
// by zero or miscounts, and it needs no explicit block-splitting code because
// the clamps keep both recurrences finite.

TEST (SelectedTridiagonalEigensolverTests, HandlesReducibleMatrices)
{
    // Two identical decoupled blocks, so every eigenvalue is exactly doubled.
    // Independent inverse iterations would return parallel vectors here.
    // expectValidEigenpairs is deliberately not used: it requires strictly
    // descending eigenvalues, and equal values are the correct answer for a
    // degenerate matrix.
    constexpr std::size_t order = 100;
    constexpr std::size_t count = 4;
    std::vector<double> diagonal (order, 3.0);
    std::vector<double> offDiagonal (order - 1, -1.0);
    offDiagonal[order / 2 - 1] = 0.0;

    const auto result = spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
        diagonal, offDiagonal, count);
    ASSERT_TRUE (result.succeeded())
        << spectrumviewer::numerics::getSymmetricTridiagonalEigenStatusDescription (
               result.status);
    ASSERT_EQ (result.eigenpairCount, count);

    for (std::size_t pair = 0; pair < count; ++pair)
    {
        if (pair > 0)
            EXPECT_GE (result.eigenvalues[pair - 1], result.eigenvalues[pair]);

        EXPECT_LT (normalizedResidual (diagonal, offDiagonal,
                                       result.getEigenvector (pair),
                                       result.eigenvalues[pair]),
                   1.0e-14);

        for (std::size_t other = 0; other <= pair; ++other)
            EXPECT_NEAR (dot (result.getEigenvector (pair),
                              result.getEigenvector (other), order),
                         pair == other ? 1.0 : 0.0, 1.0e-13);
    }
}

TEST (SelectedTridiagonalEigensolverTests, HandlesDiagonalMatrices)
{
    std::vector<double> diagonal { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
    std::vector<double> offDiagonal (diagonal.size() - 1, 0.0);

    const auto result = spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
        diagonal, offDiagonal, 4);
    ASSERT_TRUE (result.succeeded());

    for (std::size_t pair = 0; pair < result.eigenpairCount; ++pair)
    {
        EXPECT_NEAR (result.eigenvalues[pair], 10.0 - static_cast<double> (pair), 1.0e-12);

        // Each eigenvector must be a unit axis vector.
        const auto* vector = result.getEigenvector (pair);
        const auto expectedRow = diagonal.size() - 1 - pair;
        for (std::size_t row = 0; row < diagonal.size(); ++row)
            EXPECT_NEAR (std::abs (vector[row]), row == expectedRow ? 1.0 : 0.0, 1.0e-12);
    }
}

TEST (SelectedTridiagonalEigensolverTests, HandlesZeroMatrix)
{
    constexpr std::size_t order = 8;
    const std::vector<double> diagonal (order, 0.0);
    const std::vector<double> offDiagonal (order - 1, 0.0);

    const auto result = spectrumviewer::numerics::findLargestSymmetricTridiagonalEigenpairs (
        diagonal, offDiagonal, 3);
    ASSERT_TRUE (result.succeeded());

    // normalizedResidual is 0/0 here, so assert the unnormalized residual:
    // T is zero, so Tv - 0*v must be exactly zero for any v.
    for (std::size_t pair = 0; pair < result.eigenpairCount; ++pair)
    {
        EXPECT_NEAR (result.eigenvalues[pair], 0.0, 1.0e-14);

        const auto* vector = result.getEigenvector (pair);
        for (std::size_t row = 0; row < order; ++row)
        {
            auto transformed = diagonal[row] * vector[row];
            if (row > 0)
                transformed += offDiagonal[row - 1] * vector[row - 1];
            if (row + 1 < order)
                transformed += offDiagonal[row] * vector[row + 1];
            EXPECT_DOUBLE_EQ (transformed - result.eigenvalues[pair] * vector[row], 0.0);
        }
    }

    // Every direction is an eigenvector, so the only real requirement is that
    // the returned basis is orthonormal.
    for (std::size_t left = 0; left < result.eigenpairCount; ++left)
        for (std::size_t right = 0; right <= left; ++right)
            EXPECT_NEAR (dot (result.getEigenvector (left),
                              result.getEigenvector (right), order),
                         left == right ? 1.0 : 0.0, 1.0e-13);
}

TEST (SelectedTridiagonalEigensolverTests, SeparatesNearlyDegenerateEigenvalues)
{
    // Wilkinson's W+ with order 21: its top eigenvalues agree to roughly 1e-14,
    // the classic case that defeats inverse iteration without
    // reorthogonalization.
    constexpr std::size_t order = 21;
    std::vector<double> diagonal (order);
    const std::vector<double> offDiagonal (order - 1, 1.0);
    for (std::size_t index = 0; index < order; ++index)
        diagonal[index] = std::abs (static_cast<double> (index) - 10.0);

    expectValidEigenpairs (diagonal, offDiagonal, 6, 1.0e-14, 1.0e-13);
}
