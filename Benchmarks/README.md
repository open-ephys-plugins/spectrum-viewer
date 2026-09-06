# Spectrum Viewer Benchmarks

These opt-in benchmarks compare full-window materialization, per-taper fusion,
cache-tiled materialization, raw FFTW plans, and the allocation-free float
single- and equal-weighted multitaper estimators. They are informational and
are not CTest pass/fail gates. See `RESULTS.md` for measurements and their
limitations.

Configure and run locally:

```bash
cmake -S . -B BuildBenchmark -DCMAKE_BUILD_TYPE=Release \
  -DGUI_BASE_DIR=/path/to/plugin-GUI \
  -DBUILD_BENCHMARKS=ON -DBENCHMARK_NATIVE_ARCH=ON
cmake --build BuildBenchmark --target spectrum_viewer_benchmarks --parallel
./BuildBenchmark/Benchmarks/spectrum_viewer_benchmarks \
  --benchmark_filter='^(Estimator|FFTW)/'
```

The multitaper cases generate their DPSS bank and construct an
`FFTW_ESTIMATE` plan before the timed loop. Timings cover preprocessing,
channel-by-taper transforms, and calibrated equal-power aggregation.

Omit `BENCHMARK_NATIVE_ARCH` for a portable build. Never distribute a native
benchmark binary: it may contain instructions unsupported by other Open Ephys
systems.

The FFTW cases use matched system float/double libraries when both are
available. Plans are persistent and planning is outside execution timings.
`FFTWPlanning` measures cold `FFTW_MEASURE` construction separately;
`FFTWThreaded` measures FFTW-internal float threading without nesting another
thread pool.

On an i9-12900K, the eight-channel, 60,000-sample single-taper estimator took
about 0.80 ms without detrending, 1.24 ms with mean removal, and 1.34 ms with
linear detrending. Preparing five tapered rows for all eight channels took
about 0.52 ms for mean removal using fused/tiled layouts; five-taper float FFTs
took about 2.5 ms serially. FFTW-internal `plan_many` reduced the latter to
approximately 1.37, 0.78, and 0.63 ms with 2, 4, and 8 threads, but variability
increased. These figures are machine-specific and do not replace a complete
target-rig p99 measurement. Use Google Benchmark filters, repetitions, and JSON
output to archive comparable runs:

```bash
./BuildBenchmark/Benchmarks/spectrum_viewer_benchmarks \
  --benchmark_filter='Float/(Materialized|Fused|Tiled)/30kHz/N:15000/K:4/channels:8' \
  --benchmark_repetitions=30 --benchmark_report_aggregates_only=true \
  --benchmark_out=preprocessing.json --benchmark_out_format=json
```
