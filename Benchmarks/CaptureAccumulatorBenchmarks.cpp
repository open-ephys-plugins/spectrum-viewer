#include "benchmark/benchmark.h"

#include "SpectrumCaptureAccumulator.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
void captureFineEightChannels (benchmark::State& state)
{
    constexpr std::size_t channelCount = 8;
    constexpr std::size_t binCount = 30001;
    constexpr std::size_t windowCount = 30;
    std::vector<float> psd (channelCount * binCount);
    for (std::size_t index = 0; index < psd.size(); ++index)
        psd[index] = 0.01f + std::abs (std::sin (static_cast<float> (index) * 0.001f));
    spectrumviewer::SpectrumCaptureAccumulator capture (
        channelCount, binCount, windowCount);

    for (auto _ : state)
    {
        capture.reset();
        for (std::size_t window = 0; window < windowCount; ++window)
        {
            benchmark::DoNotOptimize (
                capture.add (psd.data(), channelCount, binCount));
        }
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (state.iterations()
                             * static_cast<std::int64_t> (windowCount));
    state.counters["values_per_window"] = static_cast<double> (psd.size());
    state.counters["accumulator_bytes"] = static_cast<double> (
        2 * psd.size() * sizeof (float));
}

BENCHMARK (captureFineEightChannels)
    ->Name ("Capture/Welford/8Channels/30001Bins/30Windows")
    ->Unit (benchmark::kMillisecond);
} // namespace
