# Spectrum Viewer Developer Guide

## Runtime data flow

Spectrum Viewer separates acquisition, analysis, configuration, and rendering so
that expensive work never reaches the Open Ephys acquisition callback.

```text
Open Ephys callback
  -> SampleBlockFifo
  -> SpectrumViewer worker
     -> SampleWindowAssembler
     -> MultitaperPeriodogram
     -> SpectrumDisplayReducer
  -> SpectrumFrameFifo
  -> SpectrumCanvas repaint
```

`SpectrumViewer::process()` is the only input producer. It copies each complete
callback block into preallocated, planar FIFO storage and returns. The
`SpectrumViewer` worker is the only consumer. It releases each FIFO slot after
copying the block into worker-owned history, then assembles sample-indexed
windows, sheds obsolete windows when behind, estimates PSDs, and reduces them to
the current display width. `SpectrumCanvas` consumes only complete reduced
frames.

`AsyncSpectrumAnalysis` owns a separate configuration thread. It generates DPSS
tapers, allocates a complete runtime, and creates FFTW plans without blocking the
callback, analysis worker, or message thread.

## Source map

| Area | Files | Responsibility |
| --- | --- | --- |
| Plugin lifecycle | `SpectrumViewer.*`, `SpectrumViewerEditor.*` | Parameters, acquisition lifecycle, worker scheduling, and UI controls |
| Input transport | `SampleBlockFifo.h`, `SampleWindowAssembler.h`, `BacklogSheddingPolicy.h` | Complete-block SPSC transport, continuous history, exact window cadence, and overload policy |
| Runtime preparation | `AsyncSpectrumAnalysis.*`, `SpectrumAnalysis.*` | Immutable configuration, asynchronous construction, and the worker pipeline |
| Spectral estimation | `DpssTapers.*`, `MultitaperPeriodogram.*`, `SpectrumEstimation.h` | DPSS generation, detrending, tapering, batched FFTs, and calibrated one-sided PSDs |
| Numerical support | `Numerics/SelectedTridiagonalEigensolver.*` | Plugin-private LAPACKE wrapper used only to generate selected DPSS eigenpairs |
| Display | `SpectrumDisplayReducer.*`, `SpectrumAmplitudeRange.*`, `SpectrumFrameFifo.h`, `SpectrumCanvas.*` | Linear/log bin reduction, stable dB ranges, frame publication, axes, cursors, and traces |
| Capture and comparison | `SpectrumCaptureAccumulator.*`, `SpectrumReference.*` | Non-overlapping Fine-window accumulation, variance, frozen references, and compatible comparisons |
| Correctness oracles | `ReferencePeriodogram.*`, `SingleTaperPeriodogram.*` | Independent double-precision reference and focused single-taper implementation; neither is the live pipeline |

`Tests/` contains fast component tests plus host-integrated lifecycle and canvas
tests under `Tests/Processor/`. `Benchmarks/` contains opt-in performance
measurements; see their local README files for commands and interpretation.

## Configuration and stream replacement

A profile, stream, or channel change creates a generation-tagged preparation
request. The existing runtime and input route remain live while the replacement
is built. Once ready, the worker installs the new runtime and publishes its
stream ID, global channel map, channel count, and generation as one consistent
route. The callback can therefore observe either the old route or the new route,
never a mixture. Blocks from older generations are rejected before entering new
history. Failed or superseded preparations cannot replace the active runtime.

Frames and captured references retain stream, channel, unit, frequency, and
generation metadata. Keep that identity attached when adding another output
product; labels must describe the data being drawn, not merely the latest UI
selection.

## Invariants for changes

- Do not allocate, lock, plan FFTs, notify a thread, or run DSP in
  `SpectrumViewer::process()`.
- Preserve complete-block channel alignment. Reject a block rather than enqueue
  different sample ranges for different channels.
- Keep estimator and transport storage planar: samples are contiguous within a
  channel.
- Keep PSD values linear through estimation, capture, averaging, and display
  reduction. Convert PSD to ASD or decibels only for presentation.
- Treat configurations and DPSS banks as immutable after construction.
- Preserve absolute sample positions and reset window history at discontinuities
  or generation changes.
- Keep queue overflow and backlog shedding observable through counters and frame
  metadata.

Run `ctest --test-dir Build --output-on-failure` after behavioral changes. Use
the benchmarks to justify performance-driven complexity, and record target-rig
tail latency rather than relying only on mean timings.
