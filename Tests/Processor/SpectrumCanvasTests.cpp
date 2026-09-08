#include "gtest/gtest.h"

#include "SpectrumCanvas.h"
#include "TestFixtures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace
{
using namespace std::chrono_literals;

constexpr float sampleRate = 30000.0f;
constexpr int blockSize = 600;
constexpr double twoPi = 6.283185307179586476925286766559;

std::uint64_t imageHash (const Image& image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
        {
            hash ^= static_cast<std::uint64_t> (image.getPixelAt (x, y).getARGB());
            hash *= 1099511628211ULL;
        }
    return hash;
}

void optionallyWriteImage (const Image& image, const String& name)
{
    const auto directory = SystemStats::getEnvironmentVariable (
        "SPECTRUM_VIEWER_TEST_IMAGE_DIR", {});
    if (directory.isEmpty())
        return;

    File target = File (directory).getChildFile (name);
    target.getParentDirectory().createDirectory();
    FileOutputStream output (target);
    ASSERT_TRUE (output.openedOk());
    ASSERT_TRUE (output.truncate().wasOk());
    ASSERT_TRUE (PNGImageFormat().writeImageToStream (image, output));
}

int countColourfulPixels (const Image& image, Rectangle<int> bounds)
{
    auto count = 0;
    for (int y = bounds.getY(); y < bounds.getBottom(); ++y)
        for (int x = bounds.getX(); x < bounds.getRight(); ++x)
        {
            const auto colour = image.getPixelAt (x, y);
            const auto maximum = std::max ({ colour.getRed(), colour.getGreen(), colour.getBlue() });
            const auto minimum = std::min ({ colour.getRed(), colour.getGreen(), colour.getBlue() });
            if (maximum - minimum > 20)
                ++count;
        }
    return count;
}

void expectTraceCoverage (const Image& image, Rectangle<int> drawingBounds)
{
    const auto thirdWidth = drawingBounds.getWidth() / 3;
    for (int third = 0; third < 3; ++third)
    {
        auto region = drawingBounds;
        region.setX (drawingBounds.getX() + third * thirdWidth);
        region.setWidth (third == 2
                             ? drawingBounds.getRight() - region.getX()
                             : thirdWidth);
        EXPECT_GT (countColourfulPixels (image, region), 20);
    }
}

class SpectrumCanvasTests : public testing::Test
{
protected:
    void SetUp() override
    {
        tester = std::make_unique<ProcessorTester> (
            TestSourceNodeBuilder (FakeSourceNodeParams { 8, sampleRate, 1.0f }));
        processor = tester->createProcessor<SpectrumViewer> (Plugin::Processor::SINK);
        processor->setRateAndBufferSizeDetails (sampleRate, blockSize);

        canvas = std::make_unique<SpectrumCanvas> (processor);
        canvas->setBounds (0, 0, 1200, 700);
    }

    void TearDown() override
    {
        canvas.reset();
        if (processor != nullptr
            && processor->getAnalysisReadiness() != SpectrumAnalysisReadiness::stopped)
            processor->stopAcquisition();
        processor = nullptr;
        tester.reset();
    }

    void writeBlocks (int count)
    {
        AudioBuffer<float> buffer (8, blockSize);
        for (int block = 0; block < count; ++block)
        {
            for (int channel = 0; channel < 8; ++channel)
                for (int sample = 0; sample < blockSize; ++sample)
                {
                    const auto absoluteSample = nextSample + sample;
                    const auto time = static_cast<double> (absoluteSample) / sampleRate;
                    const auto signal = 2.0 * std::sin (twoPi * 60.0 * time)
                                        + (channel == 3
                                               ? 20.0 * std::sin (twoPi * 10000.0 * time)
                                               : 0.0)
                                        + 0.05 * static_cast<double> ((absoluteSample * 17 + channel * 13) % 31);
                    buffer.setSample (channel, sample, static_cast<float> (signal));
                }
            tester->processBlock (processor, buffer);
            std::this_thread::sleep_for (2ms);
            nextSample += blockSize;
        }
    }

    bool waitUntil (const std::function<bool()>& predicate)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            canvas->refresh();
            if (predicate())
                return true;
            std::this_thread::sleep_for (2ms);
        }
        canvas->refresh();
        return predicate();
    }

    Image renderCanvas()
    {
        Image image (Image::ARGB, canvas->getWidth(), canvas->getHeight(), true);
        Graphics graphics (image);
        canvas->paintEntireComponent (graphics, true);
        return image;
    }

    std::unique_ptr<ProcessorTester> tester;
    SpectrumViewer* processor = nullptr;
    std::unique_ptr<SpectrumCanvas> canvas;
    std::int64_t nextSample = 0;
};

TEST_F (SpectrumCanvasTests, RendersFullBandMeanAndPeakThenHotSwitchesToLogAsd)
{
    ASSERT_TRUE (processor->startAcquisition());
    ASSERT_TRUE (waitUntil ([this] { return processor->hasActiveAnalysis(); }));

    auto* plot = canvas->getPlotPtr();
    for (int block = 0;
         block < 64
         && (plot->getMaximumFrequencyForTesting() != sampleRate * 0.5f
             || plot->getMeanTraceForTesting (3).empty());
         ++block)
    {
        writeBlocks (1);
        canvas->refresh();
    }
    ASSERT_TRUE (waitUntil ([plot]
    {
        return plot->getMaximumFrequencyForTesting() == sampleRate * 0.5f
               && ! plot->getMeanTraceForTesting (3).empty();
    }));
    EXPECT_EQ (plot->getFrequencyScaleForTesting(), spectrumviewer::FrequencyScale::linear);
    EXPECT_FLOAT_EQ (plot->getMinimumFrequencyForTesting(), 0.0f);
    EXPECT_FLOAT_EQ (plot->getMaximumFrequencyForTesting(), sampleRate * 0.5f);
    EXPECT_LT (plot->getFrequencyCountForTesting(), 7500u / 2u + 1u);
    EXPECT_EQ (plot->getMeanTraceForTesting (3).size(),
               plot->getFrequencyCountForTesting());
    EXPECT_EQ (plot->getPeakTraceForTesting (3).size(),
               plot->getFrequencyCountForTesting());
    const auto fixedLinearRange = plot->getPlotRangeForTesting();
    EXPECT_FLOAT_EQ (fixedLinearRange.ymin, -60.0f);
    EXPECT_FLOAT_EQ (fixedLinearRange.ymax, 60.0f);

    bool foundPreservedPeak = false;
    for (std::size_t column = 0; column < plot->getFrequencyCountForTesting(); ++column)
        if (plot->getPeakTraceForTesting (3)[column]
            > plot->getMeanTraceForTesting (3)[column])
            foundPreservedPeak = true;
    EXPECT_TRUE (foundPreservedPeak);

    const auto linearImage = renderCanvas();
    const auto linearHash = imageHash (linearImage);
    EXPECT_NE (linearHash, 0u);
    expectTraceCoverage (linearImage, plot->getDrawingBoundsForTesting());
    optionallyWriteImage (linearImage, "spectrum-linear-psd.png");

    processor->setFrequencyScale (spectrumviewer::FrequencyScale::logarithmic);
    plot->setAmplitudeDisplay (SpectrumAmplitudeDisplay::asd);
    for (int block = 0;
         block < 64
         && plot->getFrequencyScaleForTesting() != spectrumviewer::FrequencyScale::logarithmic;
         ++block)
    {
        writeBlocks (1);
        canvas->refresh();
    }
    ASSERT_TRUE (waitUntil ([plot]
    {
        return plot->getFrequencyScaleForTesting()
               == spectrumviewer::FrequencyScale::logarithmic;
    }));
    EXPECT_GT (plot->getFrequenciesForTesting().front(), 0.0f);
    EXPECT_TRUE (std::is_sorted (plot->getFrequenciesForTesting().begin(),
                                 plot->getFrequenciesForTesting().end()));
    EXPECT_TRUE (plot->getXAxisLabelForTesting().containsIgnoreCase ("log10"));
    EXPECT_TRUE (plot->getYAxisLabelForTesting().startsWith ("ASD"));
    EXPECT_TRUE (plot->getYAxisLabelForTesting().contains ("uV/sqrt(Hz)"));
    const auto logRange = plot->getPlotRangeForTesting();
    EXPECT_NEAR (logRange.xmin, std::log10 (4.0f), 1.0e-5f);
    EXPECT_NEAR (logRange.xmax, std::log10 (sampleRate * 0.5f), 1.0e-5f);
    EXPECT_FLOAT_EQ (logRange.ymin, fixedLinearRange.ymin);
    EXPECT_FLOAT_EQ (logRange.ymax, fixedLinearRange.ymax);
    const auto plottedFrequencies = plot->getPlottedFrequenciesForTesting();
    ASSERT_FALSE (plottedFrequencies.empty());
    EXPECT_GT (plottedFrequencies.front(), logRange.xmin);
    EXPECT_LT (plottedFrequencies.back(), logRange.xmax);
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        const auto& trace = plot->getMeanTraceForTesting (channel);
        EXPECT_GT (std::count_if (trace.begin(), trace.end(), [] (float value)
                                  { return std::isfinite (value); }),
                   static_cast<std::ptrdiff_t> (trace.size() / 2));
        const auto& dbTrace = plot->getDbMeanTraceForTesting (channel);
        const auto minimum = *std::min_element (dbTrace.begin(), dbTrace.end());
        const auto maximum = *std::max_element (dbTrace.begin(), dbTrace.end());
        EXPECT_LT (minimum, maximum);
        // Fixed display bounds intentionally need not contain every raw value;
        // the renderer clips traces at the plot boundary.
        EXPECT_TRUE (std::isfinite (minimum));
        EXPECT_TRUE (std::isfinite (maximum));
    }
    MessageManager::getInstance()->runDispatchLoopUntil (20);

    const auto logImage = renderCanvas();
    EXPECT_NE (imageHash (logImage), linearHash);
    expectTraceCoverage (logImage, plot->getDrawingBoundsForTesting());
    optionallyWriteImage (logImage, "spectrum-log-asd.png");
}

TEST_F (SpectrumCanvasTests, SpectralSequenceGapsAdvanceAutoRangeUsingSignalTime)
{
    auto* plot = canvas->getPlotPtr();
    plot->beginSpectrumFrame (7, 10, 8, 0.125);
    EXPECT_DOUBLE_EQ (plot->getPendingRangeElapsedSecondsForTesting(), 0.125);

    plot->beginSpectrumFrame (7, 14, 8, 0.125);
    EXPECT_DOUBLE_EQ (plot->getPendingRangeElapsedSecondsForTesting(), 0.5);

    plot->beginSpectrumFrame (7, 14, 8, 0.125);
    EXPECT_DOUBLE_EQ (plot->getPendingRangeElapsedSecondsForTesting(), 0.0);

    plot->beginSpectrumFrame (8, 2, 8, 0.125);
    EXPECT_DOUBLE_EQ (plot->getPendingRangeElapsedSecondsForTesting(), 0.125);
}
} // namespace
