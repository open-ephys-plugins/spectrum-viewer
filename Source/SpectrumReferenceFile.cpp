/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "SpectrumReferenceFile.h"

#include "SpectrumSupport.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spectrumviewer
{
namespace
{
    constexpr const char* formatName = "open-ephys-spectrum-viewer-reference";

    /** A reason the file cannot be used, worded for the user. */
    class FormatError final : public std::runtime_error
    {
    public:
        explicit FormatError (const juce::String& message)
            : std::runtime_error (message.toStdString())
        {
        }
    };

    juce::String doubleBits (double value)
    {
        std::uint64_t bits = 0;
        std::memcpy (&bits, &value, sizeof bits);
        return juce::String::toHexString (static_cast<juce::int64> (bits)).paddedLeft ('0', 16);
    }

    const char* detrendName (DetrendMode mode)
    {
        switch (mode)
        {
            case DetrendMode::none:
                return "none";
            case DetrendMode::linear:
                return "linear";
            case DetrendMode::mean:
            default:
                return "mean";
        }
    }

    /** Little-endian float32 on every platform, which is what the format
        promises and what numpy's "<f4" reads. */
    juce::String encodeFloats (const float* values, std::size_t count)
    {
        juce::MemoryOutputStream stream;
        stream.preallocate (count * sizeof (float));
        for (std::size_t index = 0; index < count; ++index)
            stream.writeFloat (values[index]);
        return juce::Base64::toBase64 (stream.getData(), stream.getDataSize());
    }

    const juce::DynamicObject& section (const juce::var& value, const juce::String& name)
    {
        if (const auto* object = value.getDynamicObject())
            return *object;
        throw FormatError ("The reference file's " + name + " section is missing or malformed.");
    }

    juce::var field (const juce::DynamicObject& object, const char* name)
    {
        if (! object.hasProperty (name))
            throw FormatError (juce::String ("The reference file is missing \"") + name + "\".");
        return object.getProperty (name);
    }

    juce::int64 integerField (const juce::DynamicObject& object, const char* name)
    {
        const auto value = field (object, name);
        if (! value.isInt() && ! value.isInt64())
            throw FormatError (juce::String ("The reference file's \"") + name
                               + "\" is not a whole number.");
        return static_cast<juce::int64> (value);
    }

    std::size_t countField (const juce::DynamicObject& object, const char* name)
    {
        const auto value = integerField (object, name);
        if (value < 0)
            throw FormatError (juce::String ("The reference file's \"") + name + "\" is negative.");
        return static_cast<std::size_t> (value);
    }

    juce::String stringField (const juce::DynamicObject& object, const char* name)
    {
        const auto value = field (object, name);
        if (! value.isString())
            throw FormatError (juce::String ("The reference file's \"") + name + "\" is not text.");
        return value.toString();
    }

    /** Reads the exact bit pattern written beside a decimal value. */
    double exactDoubleField (const juce::DynamicObject& object, const char* name)
    {
        const auto bitsName = juce::String (name) + "_bits";
        const auto text = stringField (object, bitsName.toRawUTF8());
        if (text.length() != 16 || ! text.containsOnly ("0123456789abcdefABCDEF"))
            throw FormatError ("The reference file's \"" + bitsName + "\" is malformed.");
        const auto bits = static_cast<std::uint64_t> (text.getHexValue64());
        double value = 0.0;
        std::memcpy (&value, &bits, sizeof value);
        return value;
    }

    std::vector<float> floatsField (const juce::DynamicObject& object,
                                    const char* name,
                                    std::size_t count)
    {
        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, stringField (object, name)))
            throw FormatError (juce::String ("The reference file's \"") + name
                               + "\" is not valid base64.");

        // Checked before allocating, so the declared dimensions can never ask
        // for more memory than the file itself supplies.
        const auto expectedBytes = support::checkedProduct ({ count, sizeof (float) },
                                                           "Reference data is too large");
        if (decoded.getDataSize() != expectedBytes)
            throw FormatError (juce::String ("The reference file's \"") + name
                               + "\" does not match its bin count.");

        juce::MemoryInputStream input (decoded.getData(), decoded.getDataSize(), false);
        std::vector<float> values (count);
        for (auto& value : values)
            value = input.readFloat();
        return values;
    }

    DetrendMode detrendField (const juce::DynamicObject& object)
    {
        const auto name = stringField (object, "detrend");
        if (name == "none")
            return DetrendMode::none;
        if (name == "mean")
            return DetrendMode::mean;
        if (name == "linear")
            return DetrendMode::linear;
        throw FormatError ("The reference file's detrend mode \"" + name + "\" is not recognised.");
    }
} // namespace

juce::String writeSpectrumReferenceFile (const CapturedSpectrum& spectrum,
                                         std::size_t channel,
                                         const SpectrumSourceLabels& labels)
{
    if (channel >= spectrum.getChannelCount())
        throw std::out_of_range ("The spectrum has no such channel");
    const auto& descriptor = spectrum.getDescriptor();

    juce::DynamicObject::Ptr stream = new juce::DynamicObject();
    stream->setProperty ("name", juce::String (labels.streamName));

    juce::DynamicObject::Ptr estimator = new juce::DynamicObject();
    const auto setExact = [&estimator] (const char* name, double value)
    {
        estimator->setProperty (name, value);
        estimator->setProperty (juce::String (name) + "_bits", doubleBits (value));
    };
    setExact ("sample_rate_hz", descriptor.sampleRateHz);
    setExact ("bin_width_hz", descriptor.binWidthHz);
    setExact ("time_half_bandwidth", descriptor.timeHalfBandwidth);
    estimator->setProperty ("window_samples", static_cast<juce::int64> (descriptor.windowSampleCount));
    estimator->setProperty ("hop_samples", static_cast<juce::int64> (descriptor.hopSampleCount));
    estimator->setProperty ("tapers", static_cast<juce::int64> (descriptor.taperCount));
    estimator->setProperty ("detrend", detrendName (descriptor.detrendMode));
    estimator->setProperty ("value", "psd");

    const auto& captureQuality = spectrum.getQuality();
    juce::DynamicObject::Ptr quality = new juce::DynamicObject();
    quality->setProperty ("included_windows", static_cast<juce::int64> (captureQuality.includedWindowCount));
    quality->setProperty ("target_windows", static_cast<juce::int64> (captureQuality.targetWindowCount));
    quality->setProperty ("first_sample", static_cast<juce::int64> (captureQuality.firstSample));
    quality->setProperty ("last_sample_exclusive",
                          static_cast<juce::int64> (captureQuality.lastSampleExclusive));
    quality->setProperty ("failed_windows", static_cast<juce::int64> (captureQuality.failedWindowCount));
    quality->setProperty ("shed_windows", static_cast<juce::int64> (captureQuality.shedWindowCount));
    quality->setProperty ("discontinuities", static_cast<juce::int64> (captureQuality.discontinuityCount));

    const auto hasNames = labels.channelNames.size() == spectrum.getChannelCount();
    juce::DynamicObject::Ptr source = new juce::DynamicObject();
    source->setProperty ("index", spectrum.getSourceChannelIndices()[channel]);
    source->setProperty ("units", juce::String (spectrum.getSourceChannelUnits()[channel]));
    source->setProperty ("name", hasNames ? juce::String (labels.channelNames[channel])
                                          : juce::String());

    const auto binCount = spectrum.getBinCount();
    const auto offset = channel * binCount;
    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty ("format", formatName);
    root->setProperty ("version", spectrumReferenceFileVersion);
    root->setProperty ("captured_at_unix_ms",
                       static_cast<juce::int64> (spectrum.getCapturedAtUnixMilliseconds()));
    root->setProperty ("stream", juce::var (stream.get()));
    root->setProperty ("estimator", juce::var (estimator.get()));
    root->setProperty ("quality", juce::var (quality.get()));
    root->setProperty ("channel", juce::var (source.get()));
    root->setProperty ("bins", static_cast<juce::int64> (binCount));
    root->setProperty ("mean_psd", encodeFloats (spectrum.getPlanarMeanPsd() + offset, binCount));
    root->setProperty ("sample_variance",
                       encodeFloats (spectrum.getPlanarSampleVariance() + offset, binCount));
    return juce::JSON::toString (juce::var (root.get()));
}

std::shared_ptr<const CapturedSpectrum> readSpectrumReferenceFile (const juce::String& text,
                                                                   std::uint64_t captureId,
                                                                   juce::String& error)
{
    try
    {
        juce::var parsed;
        const auto parseResult = juce::JSON::parse (text, parsed);
        if (parseResult.failed())
            throw FormatError ("The file is not valid JSON: " + parseResult.getErrorMessage());

        const auto& root = section (parsed, "top-level");
        if (! root.hasProperty ("format") || root.getProperty ("format").toString() != formatName)
            throw FormatError ("The file is not a Spectrum Viewer reference.");
        const auto version = integerField (root, "version");
        if (version > spectrumReferenceFileVersion)
            throw FormatError ("The reference was written by a newer version of Spectrum Viewer"
                               " (format " + juce::String (version) + ").");
        if (version < 1)
            throw FormatError ("The reference file's version is invalid.");

        const auto& estimator = section (field (root, "estimator"), "estimator");
        SpectrumFrameDescriptor descriptor;
        descriptor.sampleRateHz = exactDoubleField (estimator, "sample_rate_hz");
        descriptor.binWidthHz = exactDoubleField (estimator, "bin_width_hz");
        descriptor.timeHalfBandwidth = exactDoubleField (estimator, "time_half_bandwidth");
        descriptor.windowSampleCount = countField (estimator, "window_samples");
        descriptor.hopSampleCount = countField (estimator, "hop_samples");
        descriptor.taperCount = countField (estimator, "tapers");
        descriptor.detrendMode = detrendField (estimator);
        if (stringField (estimator, "value") != "psd")
            throw FormatError ("The reference file holds a spectrum kind this version cannot use.");

        const auto& qualitySection = section (field (root, "quality"), "quality");
        SpectrumCaptureQuality quality;
        quality.includedWindowCount = countField (qualitySection, "included_windows");
        quality.targetWindowCount = countField (qualitySection, "target_windows");
        quality.firstSample = integerField (qualitySection, "first_sample");
        quality.lastSampleExclusive = integerField (qualitySection, "last_sample_exclusive");
        quality.failedWindowCount = countField (qualitySection, "failed_windows");
        quality.shedWindowCount = countField (qualitySection, "shed_windows");
        quality.discontinuityCount = countField (qualitySection, "discontinuities");

        // Where the baseline was recorded. Kept to describe it; a baseline is
        // compared with any channel.
        const auto& channel = section (field (root, "channel"), "channel");
        const auto index = integerField (channel, "index");
        if (index < 0 || index > std::numeric_limits<int>::max())
            throw FormatError ("The reference file has an invalid channel index.");
        SpectrumSourceLabels labels;
        labels.streamName = stringField (section (field (root, "stream"), "stream"), "name")
                                .toStdString();
        labels.channelNames.push_back (stringField (channel, "name").toStdString());

        const auto binCount = countField (root, "bins");
        if (binCount != descriptor.windowSampleCount / 2 + 1)
            throw FormatError ("The reference file's bin count does not match its window length.");
        auto mean = floatsField (root, "mean_psd", binCount);
        auto variance = floatsField (root, "sample_variance", binCount);

        auto spectrum = std::make_shared<const CapturedSpectrum> (
            captureId,
            integerField (root, "captured_at_unix_ms"),
            descriptor,
            std::vector<int> { static_cast<int> (index) },
            std::vector<std::string> { stringField (channel, "units").toStdString() },
            std::move (mean),
            std::move (variance),
            quality,
            std::nullopt,
            std::move (labels));
        error.clear();
        return spectrum;
    }
    catch (const FormatError& formatError)
    {
        error = formatError.what();
    }
    catch (const std::invalid_argument&)
    {
        // Raised by CapturedSpectrum, which holds the file to the same rules as
        // a capture made in this session.
        error = "The reference file's values are invalid or inconsistent.";
    }
    catch (const std::exception&)
    {
        error = "The reference file could not be read.";
    }
    return nullptr;
}
} // namespace spectrumviewer
