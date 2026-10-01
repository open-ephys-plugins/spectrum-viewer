/* Tests for reading and writing one channel of a captured spectrum as a
   baseline reference file. */

#include "gtest/gtest.h"

#include "SpectrumReferenceFile.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{
using spectrumviewer::CapturedSpectrum;
using spectrumviewer::SpectrumReferenceMismatch;

constexpr std::size_t windowSamples = 16;
constexpr std::size_t binCount = windowSamples / 2 + 1;

// The second of the capture's two channels, so a file that wrote the wrong
// slice of the planar data would not pass by luck.
constexpr std::size_t exportedChannel = 1;

spectrumviewer::SpectrumFrameDescriptor makeDescriptor()
{
    spectrumviewer::SpectrumFrameDescriptor descriptor;
    // Deliberately one ulp off a round number, so a decimal round trip that
    // loses the last bit would show up as a mismatch.
    descriptor.sampleRateHz = std::nextafter (30000.0, 40000.0);
    descriptor.windowSampleCount = windowSamples;
    descriptor.hopSampleCount = windowSamples;
    descriptor.binWidthHz = descriptor.sampleRateHz / static_cast<double> (windowSamples);
    descriptor.timeHalfBandwidth = 3.0;
    descriptor.taperCount = 4;
    descriptor.detrendMode = spectrumviewer::DetrendMode::mean;
    descriptor.configurationGeneration = 17;
    return descriptor;
}

CapturedSpectrum makeCapture()
{
    std::vector<float> mean (2 * binCount);
    std::vector<float> variance (2 * binCount);
    for (std::size_t index = 0; index < mean.size(); ++index)
    {
        // Spans many binades, including values with no short decimal form.
        mean[index] = std::ldexp (1.0f + 0.1f * static_cast<float> (index),
                                  static_cast<int> (index) - 20);
        variance[index] = 0.25f * mean[index] * mean[index];
    }
    mean[binCount + 3] = 0.0f;

    spectrumviewer::SpectrumCaptureQuality quality;
    quality.includedWindowCount = 5;
    quality.targetWindowCount = 5;
    quality.firstSample = -40;
    quality.lastSampleExclusive = 9000000000;
    quality.failedWindowCount = 1;
    quality.shedWindowCount = 2;
    quality.discontinuityCount = 3;
    return { 7, 1759000000123, makeDescriptor(), { 2, 5 }, { "uV", "mV" },
             std::move (mean), std::move (variance), quality, std::uint16_t { 5 } };
}

spectrumviewer::SpectrumSourceLabels makeLabels()
{
    return { "Rhythm Data", { "CH3", "CH6" } };
}

juce::String writeFile()
{
    return spectrumviewer::writeSpectrumReferenceFile (makeCapture(), exportedChannel, makeLabels());
}

std::shared_ptr<const CapturedSpectrum> readBack (const juce::String& text)
{
    juce::String error;
    auto spectrum = spectrumviewer::readSpectrumReferenceFile (text, 11, error);
    EXPECT_EQ (spectrum == nullptr, error.isNotEmpty()) << error;
    return spectrum;
}

/** Rewrites one top-level or estimator field of a valid file. */
juce::String withEdit (const std::function<void (juce::DynamicObject&)>& change)
{
    auto parsed = juce::JSON::parse (writeFile());
    change (*parsed.getDynamicObject());
    return juce::JSON::toString (parsed);
}

bool bitsEqual (const float* left, const float* right, std::size_t count)
{
    return std::memcmp (left, right, count * sizeof (float)) == 0;
}

TEST (SpectrumReferenceFileTests, RoundTripOfTheChosenChannelIsExact)
{
    const auto original = makeCapture();
    const auto spectrum = readBack (writeFile());
    ASSERT_NE (spectrum, nullptr);

    EXPECT_TRUE (spectrum->isBaseline());
    EXPECT_EQ (spectrum->getCaptureId(), 11u);
    EXPECT_EQ (spectrum->getCapturedAtUnixMilliseconds(), 1759000000123);
    const auto& descriptor = spectrum->getDescriptor();
    const auto expected = makeDescriptor();
    EXPECT_EQ (descriptor.sampleRateHz, expected.sampleRateHz);
    EXPECT_EQ (descriptor.binWidthHz, expected.binWidthHz);
    EXPECT_EQ (descriptor.timeHalfBandwidth, expected.timeHalfBandwidth);
    EXPECT_EQ (descriptor.windowSampleCount, expected.windowSampleCount);
    EXPECT_EQ (descriptor.hopSampleCount, expected.hopSampleCount);
    EXPECT_EQ (descriptor.taperCount, expected.taperCount);
    EXPECT_EQ (descriptor.detrendMode, expected.detrendMode);

    ASSERT_EQ (spectrum->getChannelCount(), 1u);
    EXPECT_EQ (spectrum->getSourceChannelIndices(), (std::vector<int> { 5 }));
    EXPECT_EQ (spectrum->getSourceChannelUnits(), (std::vector<std::string> { "mV" }));
    const auto offset = exportedChannel * binCount;
    EXPECT_TRUE (bitsEqual (spectrum->getPlanarMeanPsd(), original.getPlanarMeanPsd() + offset,
                            binCount));
    EXPECT_TRUE (bitsEqual (spectrum->getPlanarSampleVariance(),
                            original.getPlanarSampleVariance() + offset, binCount));

    const auto& quality = spectrum->getQuality();
    EXPECT_EQ (quality.includedWindowCount, 5u);
    EXPECT_EQ (quality.firstSample, -40);
    EXPECT_EQ (quality.lastSampleExclusive, 9000000000);
    EXPECT_EQ (quality.failedWindowCount, 1u);
    EXPECT_EQ (quality.shedWindowCount, 2u);
    EXPECT_EQ (quality.discontinuityCount, 3u);

    EXPECT_EQ (spectrum->getSourceLabels().streamName, "Rhythm Data");
    EXPECT_EQ (spectrum->getSourceLabels().channelNames, (std::vector<std::string> { "CH6" }));
}

TEST (SpectrumReferenceFileTests, WritingAChannelTheCaptureLacksThrows)
{
    EXPECT_THROW (spectrumviewer::writeSpectrumReferenceFile (makeCapture(), 2, makeLabels()),
                  std::out_of_range);
}

TEST (SpectrumReferenceFileTests, BaselineMatchesOnlyTheSampleRateAndEstimator)
{
    const auto original = makeCapture();
    const auto spectrum = readBack (writeFile());
    ASSERT_NE (spectrum, nullptr);
    const auto descriptor = makeDescriptor();

    // The capture it came from is held to its stream, channels and units.
    EXPECT_EQ (original.findMismatch (descriptor, { 2, 5 }, { "uV", "mV" }, 42),
               SpectrumReferenceMismatch::stream);

    // The baseline is held up against whatever is selected.
    EXPECT_EQ (spectrum->findMismatch (descriptor, { 2, 5 }, { "uV", "mV" }, 42),
               SpectrumReferenceMismatch::none);
    EXPECT_EQ (spectrum->findMismatch (descriptor, { 0, 1, 2, 3 },
                                       { "uV", "uV", "uV", "uV" }, 3),
               SpectrumReferenceMismatch::none);

    // Everything the estimator depends on still has to match exactly.
    auto otherRate = descriptor;
    otherRate.sampleRateHz = 30000.0;
    EXPECT_EQ (spectrum->findMismatch (otherRate, { 5 }, { "mV" }, 42),
               SpectrumReferenceMismatch::sampleRate);
    auto otherTapers = descriptor;
    otherTapers.taperCount = 1;
    EXPECT_EQ (spectrum->findMismatch (otherTapers, { 5 }, { "mV" }, 42),
               SpectrumReferenceMismatch::estimator);
}

TEST (SpectrumReferenceFileTests, SpectraDecodeAsLittleEndianFloat32)
{
    // What the header promises to anyone reading the file outside the plugin,
    // for example numpy.frombuffer (base64.b64decode (s), "<f4").
    const auto original = makeCapture();
    const auto parsed = juce::JSON::parse (writeFile());
    juce::MemoryOutputStream decoded;
    ASSERT_TRUE (juce::Base64::convertFromBase64 (
        decoded, parsed.getDynamicObject()->getProperty ("mean_psd").toString()));
    ASSERT_EQ (decoded.getDataSize(), binCount * sizeof (float));

    const auto* bytes = static_cast<const std::uint8_t*> (decoded.getData());
    const auto* values = original.getPlanarMeanPsd() + exportedChannel * binCount;
    for (std::size_t index = 0; index < binCount; ++index)
    {
        std::uint32_t bits = 0;
        std::memcpy (&bits, values + index, sizeof bits);
        for (std::size_t byte = 0; byte < 4; ++byte)
            ASSERT_EQ (bytes[4 * index + byte], static_cast<std::uint8_t> (bits >> (8 * byte)));
    }
}

TEST (SpectrumReferenceFileTests, RejectsFilesItCannotUse)
{
    const auto rejects = [] (const juce::String& text)
    {
        juce::String error;
        const auto spectrum = spectrumviewer::readSpectrumReferenceFile (text, 11, error);
        return spectrum == nullptr && error.isNotEmpty();
    };

    EXPECT_TRUE (rejects ("not json"));
    EXPECT_TRUE (rejects ("{}"));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root) { root.setProperty ("format", "other"); })));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root) { root.setProperty ("version", 2); })));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root) { root.removeProperty ("mean_psd"); })));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root) { root.removeProperty ("channel"); })));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root) { root.setProperty ("bins", 8); })));

    // Data that does not match the declared bin count, including two channels'
    // worth where one is expected.
    EXPECT_TRUE (rejects (withEdit ([] (auto& root)
    {
        const float one = 1.0f;
        root.setProperty ("mean_psd", juce::Base64::toBase64 (&one, sizeof one));
    })));
    EXPECT_TRUE (rejects (withEdit ([] (auto& root)
    {
        const std::vector<float> values (2 * binCount, 1.0f);
        root.setProperty ("mean_psd",
                          juce::Base64::toBase64 (values.data(), values.size() * sizeof (float)));
    })));

    // Values a capture could never hold.
    EXPECT_TRUE (rejects (withEdit ([] (auto& root)
    {
        std::vector<float> values (binCount, 1.0f);
        values[4] = std::numeric_limits<float>::quiet_NaN();
        root.setProperty ("mean_psd",
                          juce::Base64::toBase64 (values.data(), values.size() * sizeof (float)));
    })));

    // The bit pattern is what is read, so a malformed one is rejected rather
    // than guessed at from the decimal beside it.
    EXPECT_TRUE (rejects (withEdit ([] (auto& root)
    {
        root.getProperty ("estimator").getDynamicObject()->setProperty ("sample_rate_hz_bits", "xyz");
    })));
}
} // namespace
