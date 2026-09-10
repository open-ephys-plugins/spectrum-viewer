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

TEST (SelectedTridiagonalEigensolverTests, ReportsThePinnedBackend)
{
    EXPECT_STREQ (spectrumviewer::numerics::getNumericsBackendDescription(),
                  "conda-forge OpenBLAS 0.3.34, LP64, one thread");
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
