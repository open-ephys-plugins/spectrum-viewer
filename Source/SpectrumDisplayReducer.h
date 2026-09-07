/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#ifndef SPECTRUM_DISPLAY_REDUCER_H_INCLUDED
#define SPECTRUM_DISPLAY_REDUCER_H_INCLUDED

#include <cstddef>
#include <vector>

namespace spectrumviewer
{
enum class FrequencyScale
{
    linear,
    logarithmic
};

/** Allocation-free projection of a one-sided PSD onto display columns. */
class SpectrumDisplayReducer
{
public:
    struct View
    {
        const float* getChannelMean (std::size_t channel) const noexcept;
        const float* getChannelPeak (std::size_t channel) const noexcept;

        const float* frequenciesHz = nullptr;
        std::size_t numChannels = 0;
        std::size_t numColumns = 0;
        double minimumFrequencyHz = 0.0;
        double maximumFrequencyHz = 0.0;
        FrequencyScale frequencyScale = FrequencyScale::linear;

    private:
        friend class SpectrumDisplayReducer;
        const float* means = nullptr;
        const float* peaks = nullptr;
        std::size_t columnStride = 0;
    };

    SpectrumDisplayReducer (std::size_t maximumChannels,
                            std::size_t maximumInputBins,
                            std::size_t maximumColumns);

    bool reduce (const float* planarPsd,
                 std::size_t numChannels,
                 std::size_t numBins,
                 double sampleRateHz,
                 std::size_t windowSampleCount,
                 std::size_t requestedColumns,
                 FrequencyScale scale,
                 double minimumFrequencyHz,
                 double maximumFrequencyHz) noexcept;

    const View& getView() const noexcept { return view; }

private:
    std::size_t channelCapacity;
    std::size_t inputBinCapacity;
    std::size_t columnCapacity;
    std::vector<float> means;
    std::vector<float> peaks;
    std::vector<float> frequencies;
    View view;
};
} // namespace spectrumviewer

#endif // SPECTRUM_DISPLAY_REDUCER_H_INCLUDED
