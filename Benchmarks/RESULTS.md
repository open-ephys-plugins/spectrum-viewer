# Preliminary Preprocessing Results

These measurements cover preprocessing only; they do not yet include FFT input
alignment, FFT execution, or PSD accumulation. They guide the next prototype but
do not select the final production path.

Environment: Intel Core i9-12900K, GCC 16.2.1, Release `-O3`, portable x86-64
target, one pinned CPU, 15 repetitions, and CPU frequency scaling enabled.
Inputs were wrapped, float, eight-channel windows. Times are median microseconds.

| N | K | Detrend | Full materialized | Per-taper fused | Tiled (1024) |
|---:|---:|---|---:|---:|---:|
| 500 | 3 | Mean | 2.71 | **2.56** | 2.68 |
| 500 | 3 | Linear | 5.80 | 10.64 | **5.62** |
| 4,000 | 5 | Mean | 25.52 | **24.40** | 25.19 |
| 4,000 | 5 | Linear | **49.13** | 127.52 | 49.45 |
| 15,000 | 4 | Mean | 113.93 | **99.79** | 100.81 |
| 15,000 | 4 | Linear | 200.91 | 396.65 | **186.24** |
| 60,000 | 5 | Mean | 573.20 | 527.45 | **520.95** |
| 60,000 | 5 | Linear | 910.99 | 1,905.56 | **891.67** |

Mean detrending leaves fusion and tiling close, with fusion favored for smaller
working sets and tiling at the largest size. Repeating linear detrending for each
taper is consistently poor. Tiling matches or improves full materialization at
large sizes without its channel-sized scratch buffer. Wrapped versus unwrapped
input had negligible impact, and time scaled approximately linearly from one to
eight channels.

The tiled float path was approximately 1.3–1.8 times faster than tiled double for
mean detrending in this matrix. A separate `-march=native` build emitted AVX2/FMA
instructions but did not improve timings consistently, which is compatible with
a cache/memory-bound kernel and frequency-scaling noise. Repeat on supported
platforms, sweep tile size, and include matched float/double FFT execution before
making the final implementation decision.

## Float FFT and estimator follow-up

The following measurements used matched system FFTW float and double builds on
the same i9-12900K. Persistent `plan_many` float transforms took about 0.49 ms
for 32 transforms of 15,000 samples and 2.65 ms for 40 transforms of 60,000
samples. Persistent individual plans were 10% and 6% faster respectively, so
batching is an API and planning convenience rather than an automatic execution
win. Power-of-two controls showed the same pattern.

FFTW-internal threading changed that result for the 40-by-60,000 batch:
`plan_many` took approximately 1.37 ms with two threads, 0.78 ms with four, and
0.63 ms with eight, though run-to-run variability increased. Threading separate
plans was slower because each transform repeatedly entered the FFTW thread
team. These results justify keeping the wrapper's backend private and deferring
a public threading API until end-to-end p99 measurements show a need.

For eight channels and 60,000 samples, the complete float single-taper path
(detrending, tapering, FFT, and PSD scaling) took approximately 0.80 ms without
detrending, 1.26 ms with mean removal, and 1.36 ms with linear detrending.
Wrapped and contiguous inputs were effectively equivalent. Combined with the
five-taper preprocessing result above, this ballparks a serial five-taper
update at only a few milliseconds against the proposed 500 ms fine-mode hop.

## Equal-weighted multitaper estimator

The implemented cache-tiled estimator was measured end to end with persistent
`FFTW_ESTIMATE` plans, including trend estimation, five-taper materialization,
40 float transforms, actual taper-energy normalization, double-precision power
accumulation, and one-sided endpoint scaling. For eight channels at N=60,000,
ten-repetition median wall times were 4.65 ms for no detrending, 5.01 ms for
mean removal, and 5.12 ms for linear detrending. Standard deviations were 0.3%
to 1.2% with CPU frequency scaling enabled. These are local throughput results,
not target-rig p99 latency or evidence for a response-profile default.
