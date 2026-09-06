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
