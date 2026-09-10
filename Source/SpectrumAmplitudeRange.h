/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef SPECTRUM_AMPLITUDE_RANGE_H_INCLUDED
#define SPECTRUM_AMPLITUDE_RANGE_H_INCLUDED

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
enum class AmplitudeRangeMode
{
    automatic = 1,
    fixed = 2
};

struct DecibelRange
{
    float minimum = -25.0f;
    float maximum = 25.0f;
};

/**
    Holds a fixed dB range or slowly follows robust bounds from complete frames.

    Automatic updates are driven by elapsed signal time. The first valid frame
    initializes immediately; later frames use a faster outward time constant and
    a slower inward time constant so transients become visible without making the
    noise floor pump from frame to frame.
*/
class SpectrumAmplitudeRange
{
public:
    SpectrumAmplitudeRange() = default;

    void setMode (AmplitudeRangeMode newMode) noexcept;
    AmplitudeRangeMode getMode() const noexcept { return mode; }
    void resetAutomatic() noexcept { automaticRangeIsValid = false; }
    bool hasAutomaticRange() const noexcept { return automaticRangeIsValid; }

    bool setFixedRange (float minimumDb, float maximumDb) noexcept;
    DecibelRange getFixedRange() const noexcept { return fixedRange; }
    DecibelRange getCurrentRange() const noexcept;

    DecibelRange update (const std::vector<std::vector<float>>& meanDb,
                         const std::vector<std::vector<float>>& peakDb,
                         std::size_t channelCount,
                         double elapsedSignalSeconds);

private:
    static constexpr float minimumSpanDb = 20.0f;
    static constexpr float paddingDb = 3.0f;
    static constexpr float deadbandDb = 1.0f;
    static constexpr double outwardTimeConstantSeconds = 2.0;
    static constexpr double inwardTimeConstantSeconds = 15.0;

    bool fitTarget (const std::vector<std::vector<float>>& meanDb,
                    const std::vector<std::vector<float>>& peakDb,
                    std::size_t channelCount,
                    DecibelRange& target);
    static float follow (float current,
                         float target,
                         bool movingOutward,
                         double elapsedSignalSeconds) noexcept;

    AmplitudeRangeMode mode = AmplitudeRangeMode::automatic;
    DecibelRange fixedRange;
    DecibelRange automaticRange;
    bool automaticRangeIsValid = false;
    std::vector<float> finiteMeans;
};
} // namespace spectrumviewer

#endif // SPECTRUM_AMPLITUDE_RANGE_H_INCLUDED
