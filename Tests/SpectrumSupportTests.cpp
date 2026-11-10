/* Tests for the helpers the estimators and transport buffers share. */

#include "gtest/gtest.h"

#include "SpectrumSupport.h"

#include <cstddef>
#include <limits>
#include <stdexcept>

namespace
{
namespace support = spectrumviewer::support;
constexpr auto maximumSize = std::numeric_limits<std::size_t>::max();

TEST (SpectrumSupportTests, CheckedProductMultipliesAndDetectsOverflow)
{
    EXPECT_EQ (support::checkedProduct ({ 3, 4, 5 }, "unused"), 60u);
    EXPECT_EQ (support::checkedProduct ({ maximumSize, 1 }, "unused"), maximumSize);

    // The overflow can hide in any pair, not only the first.
    EXPECT_THROW (support::checkedProduct ({ maximumSize / 2 + 1, 2 }, "overflow"),
                  std::length_error);
    EXPECT_THROW (support::checkedProduct ({ 2, 3, maximumSize / 5 }, "overflow"),
                  std::length_error);
    EXPECT_THROW (support::checkedProduct<std::invalid_argument> ({ maximumSize, 2 }, "overflow"),
                  std::invalid_argument);
}

TEST (SpectrumSupportTests, ZeroDimensionsAreEmptyUnlessTheyMustBePositive)
{
    // A zero factor must neither divide by zero in the overflow test nor mask
    // the fact that the product is empty.
    EXPECT_EQ (support::checkedProduct ({ 0, maximumSize, maximumSize }, "unused"), 0u);
    EXPECT_EQ (support::checkedProduct ({ maximumSize, 0 }, "unused"), 0u);

    EXPECT_THROW (support::checkedPositiveProduct ({ 4, 0 }, "empty"), std::invalid_argument);
    EXPECT_THROW (support::checkedPositiveProduct ({ maximumSize, 2 }, "overflow"),
                  std::invalid_argument);
    EXPECT_EQ (support::checkedPositiveProduct ({ 4, 2 }, "unused"), 8u);
}

TEST (SpectrumSupportTests, IntegerAndFifoSizesStayInRange)
{
    constexpr auto maximumInt = static_cast<std::size_t> (std::numeric_limits<int>::max());
    EXPECT_EQ (support::checkedPositiveInt (maximumInt, "unused"),
               std::numeric_limits<int>::max());
    EXPECT_THROW (support::checkedPositiveInt (0, "zero"), std::invalid_argument);
    EXPECT_THROW (support::checkedPositiveInt (maximumInt + 1, "too large"),
                  std::invalid_argument);

    // AbstractFifo keeps one slot free, so a FIFO of N slots needs size N + 1.
    EXPECT_EQ (support::checkedFifoSize (3, "unused"), 4);
    EXPECT_THROW (support::checkedFifoSize (0, "zero"), std::invalid_argument);
    EXPECT_THROW (support::checkedFifoSize (maximumInt, "too large"), std::invalid_argument);
}

TEST (SpectrumSupportTests, OneSidedFoldingSkipsDcAndNyquistOnly)
{
    // Even length: DC and Nyquist have no negative-frequency mirror.
    EXPECT_FALSE (support::isFoldedOneSidedBin (0, 8));
    EXPECT_TRUE (support::isFoldedOneSidedBin (1, 8));
    EXPECT_TRUE (support::isFoldedOneSidedBin (3, 8));
    EXPECT_FALSE (support::isFoldedOneSidedBin (4, 8));

    // Odd length: there is no Nyquist bin, so the last bin is folded.
    EXPECT_FALSE (support::isFoldedOneSidedBin (0, 7));
    EXPECT_TRUE (support::isFoldedOneSidedBin (3, 7));
}

TEST (SpectrumSupportTests, RecognisesOnlyTheDefinedDetrendModes)
{
    using spectrumviewer::DetrendMode;
    EXPECT_TRUE (support::isValidDetrendMode (DetrendMode::none));
    EXPECT_TRUE (support::isValidDetrendMode (DetrendMode::mean));
    EXPECT_TRUE (support::isValidDetrendMode (DetrendMode::linear));
    EXPECT_FALSE (support::isValidDetrendMode (static_cast<DetrendMode> (42)));
}
} // namespace
