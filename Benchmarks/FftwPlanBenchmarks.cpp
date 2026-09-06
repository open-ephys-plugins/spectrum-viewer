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

#include <benchmark/benchmark.h>
#include <fftw3.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace
{
template <typename Sample>
struct Fftw;

template <>
struct Fftw<float>
{
    using Complex = fftwf_complex;
    using Plan = fftwf_plan;

    static float* allocateReal (std::size_t count) { return fftwf_alloc_real (count); }
    static Complex* allocateComplex (std::size_t count) { return fftwf_alloc_complex (count); }
    static void release (void* pointer) { fftwf_free (pointer); }
    static void destroy (Plan plan) { fftwf_destroy_plan (plan); }
    static void execute (Plan plan) { fftwf_execute (plan); }
    static void cleanup() { fftwf_cleanup(); }
#if SPECTRUM_VIEWER_FFTW_THREADS
    static bool initializeThreads() { return fftwf_init_threads() != 0; }
    static void setThreadCount (int count) { fftwf_plan_with_nthreads (count); }
    static void cleanupThreads() { fftwf_cleanup_threads(); }
#endif
    static Plan planOne (int size, float* input, Complex* output)
    {
        return fftwf_plan_dft_r2c_1d (size, input, output, FFTW_MEASURE);
    }
    static Plan planMany (int size, int count, float* input, Complex* output)
    {
        const int dimensions[] { size };
        return fftwf_plan_many_dft_r2c (
            1, dimensions, count, input, nullptr, 1, size, output, nullptr, 1, size / 2 + 1, FFTW_MEASURE);
    }
};

template <>
struct Fftw<double>
{
    using Complex = fftw_complex;
    using Plan = fftw_plan;

    static double* allocateReal (std::size_t count) { return fftw_alloc_real (count); }
    static Complex* allocateComplex (std::size_t count) { return fftw_alloc_complex (count); }
    static void release (void* pointer) { fftw_free (pointer); }
    static void destroy (Plan plan) { fftw_destroy_plan (plan); }
    static void execute (Plan plan) { fftw_execute (plan); }
    static void cleanup() { fftw_cleanup(); }
#if SPECTRUM_VIEWER_FFTW_THREADS
    static bool initializeThreads() { return fftw_init_threads() != 0; }
    static void setThreadCount (int count) { fftw_plan_with_nthreads (count); }
    static void cleanupThreads() { fftw_cleanup_threads(); }
#endif
    static Plan planOne (int size, double* input, Complex* output)
    {
        return fftw_plan_dft_r2c_1d (size, input, output, FFTW_MEASURE);
    }
    static Plan planMany (int size, int count, double* input, Complex* output)
    {
        const int dimensions[] { size };
        return fftw_plan_many_dft_r2c (
            1, dimensions, count, input, nullptr, 1, size, output, nullptr, 1, size / 2 + 1, FFTW_MEASURE);
    }
};

template <typename Sample, bool batched>
class Fixture
{
public:
    Fixture (int transformSize, int transformCount, int threadCount = 1)
        : size (transformSize),
          count (transformCount),
          bins (size / 2 + 1),
          input (Fftw<Sample>::allocateReal (static_cast<std::size_t> (size * count))),
          output (Fftw<Sample>::allocateComplex (static_cast<std::size_t> (bins * count)))
    {
        if (size <= 0 || count <= 0 || input == nullptr || output == nullptr)
        {
            Fftw<Sample>::release (output);
            Fftw<Sample>::release (input);
            throw std::runtime_error ("Unable to allocate FFTW benchmark fixture");
        }

        for (int transform = 0; transform < count; ++transform)
            for (int sample = 0; sample < size; ++sample)
                input[transform * size + sample] = static_cast<Sample> (
                    std::sin (0.01 * static_cast<double> (sample + transform)));

#if SPECTRUM_VIEWER_FFTW_THREADS
        if (threadCount > 1)
        {
            if (! Fftw<Sample>::initializeThreads())
                throw std::runtime_error ("Unable to initialize FFTW threads");
            threadsInitialized = true;
            Fftw<Sample>::setThreadCount (threadCount);
        }
#else
        if (threadCount > 1)
            throw std::runtime_error ("FFTW thread support is unavailable");
#endif

        if constexpr (batched)
        {
            plans.push_back (Fftw<Sample>::planMany (size, count, input, output));
        }
        else
        {
            plans.reserve (static_cast<std::size_t> (count));
            for (int transform = 0; transform < count; ++transform)
            {
                plans.push_back (Fftw<Sample>::planOne (
                    size, input + transform * size, output + transform * bins));
            }
        }

        for (const auto plan : plans)
        {
            if (plan == nullptr)
            {
                for (const auto createdPlan : plans)
                    if (createdPlan != nullptr)
                        Fftw<Sample>::destroy (createdPlan);
                Fftw<Sample>::release (output);
                Fftw<Sample>::release (input);
#if SPECTRUM_VIEWER_FFTW_THREADS
                if (threadsInitialized)
                    Fftw<Sample>::cleanupThreads();
#endif
                throw std::runtime_error ("Unable to create FFTW benchmark plan");
            }
        }
    }

    ~Fixture()
    {
        for (const auto plan : plans)
            if (plan != nullptr)
                Fftw<Sample>::destroy (plan);
        Fftw<Sample>::release (output);
        Fftw<Sample>::release (input);
#if SPECTRUM_VIEWER_FFTW_THREADS
        if (threadsInitialized)
            Fftw<Sample>::cleanupThreads();
#endif
    }

    void execute()
    {
        for (const auto plan : plans)
            Fftw<Sample>::execute (plan);
    }

    const typename Fftw<Sample>::Complex* getOutput() const noexcept { return output; }

private:
    const int size;
    const int count;
    const int bins;
    Sample* input;
    typename Fftw<Sample>::Complex* output;
    std::vector<typename Fftw<Sample>::Plan> plans;
#if SPECTRUM_VIEWER_FFTW_THREADS
    bool threadsInitialized = false;
#endif
};

template <typename Sample, bool batched>
void runFftBenchmark (benchmark::State& state)
{
    const auto size = static_cast<int> (state.range (0));
    const auto count = static_cast<int> (state.range (1));
    Fixture<Sample, batched> fixture (size, count);

    for (auto _ : state)
    {
        fixture.execute();
        benchmark::DoNotOptimize (fixture.getOutput());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (state.iterations() * static_cast<std::int64_t> (count));
    state.counters["input_bytes"] = static_cast<double> (sizeof (Sample))
                                    * static_cast<double> (size * count);
    state.counters["transforms"] = static_cast<double> (count);
}

template <typename Sample, bool batched>
void runPlanConstructionBenchmark (benchmark::State& state)
{
    const auto size = static_cast<int> (state.range (0));
    const auto count = static_cast<int> (state.range (1));

    for (auto _ : state)
    {
        Fftw<Sample>::cleanup();
        Fixture<Sample, batched> fixture (size, count);
        benchmark::DoNotOptimize (fixture.getOutput());
    }
    Fftw<Sample>::cleanup();
}

#if SPECTRUM_VIEWER_FFTW_THREADS
template <bool batched>
void runThreadedFloatBenchmark (benchmark::State& state)
{
    const auto size = static_cast<int> (state.range (0));
    const auto count = static_cast<int> (state.range (1));
    const auto threads = static_cast<int> (state.range (2));
    Fixture<float, batched> fixture (size, count, threads);

    for (auto _ : state)
    {
        fixture.execute();
        benchmark::DoNotOptimize (fixture.getOutput());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed (state.iterations() * static_cast<std::int64_t> (count));
    state.counters["threads"] = static_cast<double> (threads);
    state.counters["transforms"] = static_cast<double> (count);
}
#endif

void addFftCases (benchmark::internal::Benchmark* benchmark)
{
    // 2 kHz and 30 kHz versions of the provisional Fast, Balanced, and Fine profiles.
    for (const auto sizeAndTapers : { std::pair<int, int> { 500, 3 },
                                      std::pair<int, int> { 1000, 4 },
                                      std::pair<int, int> { 1024, 4 },
                                      std::pair<int, int> { 4000, 5 },
                                      std::pair<int, int> { 7500, 3 },
                                      std::pair<int, int> { 15000, 4 },
                                      std::pair<int, int> { 16384, 4 },
                                      std::pair<int, int> { 60000, 5 },
                                      std::pair<int, int> { 65536, 5 } })
    {
        for (const auto channels : { 1, 8 })
            benchmark->Args ({ sizeAndTapers.first, sizeAndTapers.second * channels });
    }

    benchmark->ArgNames ({ "N", "channel_x_taper" })
        ->Unit (benchmark::kMicrosecond)
        ->UseRealTime();
}

void addPlanningCases (benchmark::internal::Benchmark* benchmark)
{
    benchmark->Args ({ 1000, 32 })
        ->Args ({ 15000, 32 })
        ->Args ({ 60000, 40 })
        ->ArgNames ({ "N", "channel_x_taper" })
        ->Unit (benchmark::kMillisecond)
        ->UseRealTime()
        ->Iterations (1);
}

#if SPECTRUM_VIEWER_FFTW_THREADS
void addThreadedCases (benchmark::internal::Benchmark* benchmark)
{
    for (const auto sizeAndCount : { std::pair<int, int> { 7500, 24 },
                                     std::pair<int, int> { 15000, 32 },
                                     std::pair<int, int> { 60000, 40 } })
    {
        for (const auto threads : { 2, 4, 8 })
            benchmark->Args ({ sizeAndCount.first, sizeAndCount.second, threads });
    }

    benchmark->ArgNames ({ "N", "channel_x_taper", "threads" })
        ->Unit (benchmark::kMicrosecond)
        ->UseRealTime();
}
#endif

BENCHMARK_TEMPLATE (runFftBenchmark, float, true)->Name ("FFTW/Float/PlanMany")->Apply (addFftCases);
BENCHMARK_TEMPLATE (runFftBenchmark, float, false)->Name ("FFTW/Float/SeparatePlans")->Apply (addFftCases);
BENCHMARK_TEMPLATE (runFftBenchmark, double, true)->Name ("FFTW/Double/PlanMany")->Apply (addFftCases);
BENCHMARK_TEMPLATE (runFftBenchmark, double, false)->Name ("FFTW/Double/SeparatePlans")->Apply (addFftCases);
BENCHMARK_TEMPLATE (runPlanConstructionBenchmark, float, true)->Name ("FFTWPlanning/Float/PlanMany")->Apply (addPlanningCases);
BENCHMARK_TEMPLATE (runPlanConstructionBenchmark, float, false)->Name ("FFTWPlanning/Float/SeparatePlans")->Apply (addPlanningCases);
BENCHMARK_TEMPLATE (runPlanConstructionBenchmark, double, true)->Name ("FFTWPlanning/Double/PlanMany")->Apply (addPlanningCases);
BENCHMARK_TEMPLATE (runPlanConstructionBenchmark, double, false)->Name ("FFTWPlanning/Double/SeparatePlans")->Apply (addPlanningCases);
#if SPECTRUM_VIEWER_FFTW_THREADS
BENCHMARK_TEMPLATE (runThreadedFloatBenchmark, true)->Name ("FFTWThreaded/Float/PlanMany")->Apply (addThreadedCases);
BENCHMARK_TEMPLATE (runThreadedFloatBenchmark, false)->Name ("FFTWThreaded/Float/SeparatePlans")->Apply (addThreadedCases);
#endif
} // namespace
