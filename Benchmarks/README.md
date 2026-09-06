# Spectrum Viewer Benchmarks

These opt-in benchmarks compare full-window materialization, per-taper fusion,
and cache-tiled materialization. They are informational and are not CTest
pass/fail gates. See `RESULTS.md` for the initial local measurements and their
limitations.

Configure a portable Release build:

```sh
cmake -S . -B BuildBenchmark -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build BuildBenchmark --target spectrum_viewer_benchmarks --parallel
./BuildBenchmark/Benchmarks/spectrum_viewer_benchmarks
```

To measure the build machine's available instruction set, configure a separate
directory with `-DBENCHMARK_NATIVE_ARCH=ON`. Never distribute that binary: it
may contain instructions unsupported by other Open Ephys systems.

The argument columns are window samples, taper count, channel count, circular
history offset (`0` is unwrapped), and detrend mode (`0=None`, `1=Mean`,
`2=Linear`). Use Google Benchmark filters and JSON output to select or archive
runs, for example:

```sh
./BuildBenchmark/Benchmarks/spectrum_viewer_benchmarks \
  --benchmark_filter='Float/(Materialized|Fused|Tiled)/30kHz/N:15000/K:4/channels:8' \
  --benchmark_repetitions=30 --benchmark_report_aggregates_only=true \
  --benchmark_out=preprocessing.json --benchmark_out_format=json
```
