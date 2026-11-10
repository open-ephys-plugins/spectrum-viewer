#include "benchmark/benchmark.h"

#include "SpectrumDisplayReducer.h"
#include "SpectrumReference.h"

#include <cstddef>
#include <vector>

namespace
{
void referenceComparisonFineEightChannels (benchmark::State& state)
{
    constexpr std::size_t channelCount = 8;
    constexpr std::size_t binCount = 30001;
    constexpr std::size_t columnCount = 800;
    constexpr std::size_t windowSamples = 60000;
    constexpr double sampleRateHz = 30000.0;
    std::vector<float> current (channelCount * binCount);
    std::vector<float> reference (current.size());
    std::vector<float> delta (channelCount * columnCount);
    for (std::size_t index = 0; index < current.size(); ++index)
    {
        current[index] = 1.0f + static_cast<float> (index % 101) * 0.01f;
        reference[index] = 0.75f + static_cast<float> (index % 97) * 0.01f;
    }
    spectrumviewer::SpectrumDisplayReducer currentReducer (
        channelCount, binCount, columnCount);
    spectrumviewer::SpectrumDisplayReducer referenceReducer (
        channelCount, binCount, columnCount);

    for (auto _ : state)
    {
        if (! currentReducer.reduce (
            current.data(), channelCount, binCount, sampleRateHz,
            windowSamples, columnCount, spectrumviewer::FrequencyScale::logarithmic,
            0.5, sampleRateHz * 0.5)
            || ! referenceReducer.reduce (
            reference.data(), channelCount, binCount, sampleRateHz,
            windowSamples, columnCount, spectrumviewer::FrequencyScale::logarithmic,
            0.5, sampleRateHz * 0.5)
            || ! spectrumviewer::computeDecibelDelta (
                currentReducer.getView().getChannelMean (0),
                referenceReducer.getView().getChannelMean (0),
                delta.data(), delta.size()))
        {
            state.SkipWithError ("Reference comparison rejected valid inputs");
            break;
        }
        benchmark::DoNotOptimize (delta.data());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed (state.iterations()
                             * static_cast<std::int64_t> (channelCount));
}

BENCHMARK (referenceComparisonFineEightChannels)
    ->Name ("Reference/ReduceAndDelta/8Channels/30001Bins/800LogColumns")
    ->Unit (benchmark::kMicrosecond);
} // namespace
