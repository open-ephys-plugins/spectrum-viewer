/*
    ------------------------------------------------------------------

    This file is part of a plugin for the Open Ephys GUI
    Copyright (C) 2026 Open Ephys

    ------------------------------------------------------------------
*/

#include <benchmark/benchmark.h>

#include "SpectrumCanvas.h"
#include "TestFixtures.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <thread>

namespace
{
using namespace std::chrono_literals;

constexpr float sampleRate = 30000.0f;
constexpr int blockSize = 15000;
constexpr double twoPi = 6.283185307179586476925286766559;

double percentile99 (const std::vector<double>& values)
{
    auto sorted = values;
    std::sort (sorted.begin(), sorted.end());
    const auto index = static_cast<std::size_t> (
        std::ceil (0.99 * static_cast<double> (sorted.size()))) - 1;
    return sorted[index];
}

SpectrumAnalysisProfile profileFor (std::int64_t value)
{
    switch (value)
    {
        case 1:
            return SpectrumAnalysisProfile::fast;
        case 2:
            return SpectrumAnalysisProfile::balanced;
        case 3:
            return SpectrumAnalysisProfile::fine;
        default:
            throw std::invalid_argument ("Unknown spectrum analysis profile");
    }
}

void runGuiRepaint (benchmark::State& state)
{
    auto tester = std::make_unique<ProcessorTester> (
        TestSourceNodeBuilder (FakeSourceNodeParams { 8, sampleRate, 1.0f }));
    auto* processor = tester->createProcessor<SpectrumViewer> (Plugin::Processor::SINK);
    processor->setRateAndBufferSizeDetails (sampleRate, blockSize);
    processor->setAnalysisProfile (profileFor (state.range (0)));
    processor->setFrequencyScale (
        state.range (1) == 0 ? spectrumviewer::FrequencyScale::linear
                             : spectrumviewer::FrequencyScale::logarithmic);

    auto canvas = std::make_unique<SpectrumCanvas> (processor);
    canvas->setBounds (0, 0, 2048, 900);
    if (! processor->startAcquisition())
    {
        state.SkipWithError ("Unable to start Spectrum Viewer acquisition");
        return;
    }

    const auto readinessDeadline = std::chrono::steady_clock::now() + 10s;
    while (! processor->hasActiveAnalysis()
           && std::chrono::steady_clock::now() < readinessDeadline)
        std::this_thread::sleep_for (1ms);
    if (! processor->hasActiveAnalysis())
    {
        state.SkipWithError ("Analysis preparation timed out");
        processor->stopAcquisition();
        return;
    }

    AudioBuffer<float> buffer (8, blockSize);
    for (int channel = 0; channel < 8; ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto time = static_cast<double> (sample) / sampleRate;
            const auto value = std::sin (twoPi * 60.0 * time)
                               + (channel == 3
                                      ? 10.0 * std::sin (twoPi * 10000.0 * time)
                                      : 0.0);
            buffer.setSample (channel, sample, static_cast<float> (value));
        }

    auto* plot = canvas->getPlotPtr();
    const auto frameDeadline = std::chrono::steady_clock::now() + 10s;
    while ((plot->getMaximumFrequencyForTesting() != sampleRate * 0.5f
            || plot->getMeanTraceForTesting (0).size() < 1000)
           && std::chrono::steady_clock::now() < frameDeadline)
    {
        tester->processBlock (processor, buffer);
        std::this_thread::sleep_for (10ms);
        canvas->refresh();
    }
    if (plot->getMaximumFrequencyForTesting() != sampleRate * 0.5f
        || plot->getMeanTraceForTesting (0).size() < 1000)
    {
        state.SkipWithError ("Display frame preparation timed out");
        processor->stopAcquisition();
        return;
    }

    Image image (Image::ARGB, canvas->getWidth(), canvas->getHeight(), true);
    Graphics graphics (image);
    for (auto _ : state)
    {
        canvas->paintEntireComponent (graphics, true);
        benchmark::DoNotOptimize (image.getPixelAt (image.getWidth() / 2,
                                                     image.getHeight() / 2));
        benchmark::ClobberMemory();
    }

    state.counters["pixels"] = static_cast<double> (image.getWidth() * image.getHeight());
    state.counters["columns"] = static_cast<double> (plot->getFrequencyCountForTesting());
    processor->stopAcquisition();
    canvas.reset();
    processor = nullptr;
    tester.reset();
}

void addGuiRepaintCases (benchmark::internal::Benchmark* benchmark)
{
    for (const auto profile : { 1, 2, 3 })
        for (const auto logAxis : { 0, 1 })
            benchmark->Args ({ profile, logAxis });
    benchmark->ArgNames ({ "profile", "log_axis" })
        ->Unit (benchmark::kMillisecond)
        ->UseRealTime()
        ->ComputeStatistics ("p99", percentile99);
}

BENCHMARK (runGuiRepaint)
    ->Name ("GUI/SoftwareRepaint/8Channels/2048x900")
    ->Apply (addGuiRepaintCases);
} // namespace
