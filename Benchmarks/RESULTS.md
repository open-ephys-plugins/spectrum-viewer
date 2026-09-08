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

## Worker pipeline follow-up

The worker-side pipeline benchmark adds the circular-history copy and window
assembly needed for production. With the proposed Fine dimensions (N=60,000,
K=5, NW=3, 15,000-sample/0.5 s hop), eight channels, and linear detrending, its
ten-repetition median was 5.46 ms (0.37% coefficient of variation). DPSS
generation and FFTW planning occurred before timing. The input-FIFO publication
copy and display-frame copy remain outside this measurement; both are bounded
planar copies covered by transport tests. This is still local throughput, not a
target-rig p99 result.

## Worker-to-display follow-up

The integrated benchmark now continues through full-band reduction to 1,920
display columns and copies area-weighted means, peak envelopes, and frequency
coordinates into simulated publication storage. Five-repetition medians for
eight channels with linear detrending were 0.453/0.470 ms (Fast linear/log),
1.063/1.077 ms (Balanced), and 6.126/6.150 ms (Fine). Frequency mapping is a
small part of total worker cost. CPU scaling was enabled, so these measurements
are informational. They exclude the input FIFO copy, FIFO index publication,
GUI-thread model copy and JUCE paint; an instrumented graphical target-rig run
is still required for repaint p50/p99 and responsiveness while resizing.

## GUI software repaint follow-up

The real eight-channel `SpectrumCanvas` was rendered at 2,048 x 900 with 1,640
display columns. The host `XYLine` initially issued one graphics call per line
segment: five-repetition medians were 41--50 ms across profiles and axes. A
plugin-local `XYLine` subclass now submits one JUCE path per trace while retaining
the host plot's axes, grid, ownership, and clearing behavior.

In a 30-repetition run with the fixed -60 to 60 dB default amplitude range,
linear-axis repaint p50/p99 was 4.64/15.1 ms Fast, 3.15/4.96 ms Balanced, and
3.29/8.01 ms Fine. Log-axis p50/p99 was 2.75/3.99, 5.54/10.7, and 2.99/4.42 ms
respectively. Profile differences here come from trace geometry and host
scheduling, not different column counts. CPU scaling was enabled and the Fast
linear tail was noisy; these are diagnostic software-raster results, not
target-rig acceptance figures.

The robust auto-amplitude fit and time update took 43.5 microseconds median for
eight 1,920-column mean/peak traces over 30 repetitions. This work runs only for
new spectral frames, never for repaint-only callbacks.

The matching worker-to-display p50/p99 was 0.451/0.457 ms Fast, 1.043/1.063 ms
Balanced, and 5.673/5.893 ms Fine on a linear axis. Log reduction remained
within 0.02 ms of linear at p50. Actual display latency also includes FIFO
publication, GUI refresh/model conversion, compositor work, and scheduling.

## Low-variance capture accumulator

An eight-channel Fine capture has 30,001 bins per channel at 30 kHz. Two float
arrays for its full-resolution running mean and Welford accumulator occupy
1.92 MB. A 30-window capture update benchmark completed all 30 planar updates
in 6.58 ms median, or approximately 0.22 ms per accepted two-second window.
This local 30-repetition run had 1.64% coefficient of variation with CPU
scaling enabled. It excludes the multitaper estimate and display reduction,
which are reported separately above.

## Session reference comparison

For eight Fine-profile channels at 30 kHz, reducing both current and reference
30,001-bin PSDs to 800 logarithmic display columns and computing all 6,400 dB
deltas took 0.780 ms median over five repetitions (0.43% coefficient of
variation). CPU scaling was enabled. This measures the complete comparison
reduction; relative to the ordinary display path, only the second reduction and
dB subtraction are additional work. It remains far below the 500 ms Fine live
hop and two-second capture hop, but target-rig repaint and p99 validation remain.
