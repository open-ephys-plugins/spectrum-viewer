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
     -> AperiodicSpectrumBaseline
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

## Thread ownership

Four execution contexts cooperate, but each mutable DSP object has one owner:

| Context | Owns or changes | May communicate through |
| --- | --- | --- |
| Acquisition callback | One input-block publication at a time | `SampleBlockFifo` and the read-only input-route snapshot |
| Analysis worker | Active runtime, sample history, estimator state, captures, and references | Input FIFO, output FIFO, atomics, and configuration results |
| Configuration thread | Pending build request and destruction of retired runtimes | A mutex-protected, coalescing mailbox |
| JUCE message thread | Parameters, editor state, canvas state, and repaint | Atomics, preparation requests, reference commands, and newest output frame |

The acquisition callback and analysis worker form the input FIFO's single
producer and single consumer. The analysis worker and message thread have the
same roles for each runtime's frame FIFO. Do not add a second producer or
consumer without replacing the transport contract.

The configuration mailbox may lock because neither participant is the
acquisition callback. Runtime construction occurs after releasing that lock.
The canvas obtains the runtime used for display with `try_lock`; it skips an
update instead of blocking if the worker is installing a replacement.

## Source map

| Area | Files | Responsibility |
| --- | --- | --- |
| Plugin lifecycle | `SpectrumViewer.*`, `SpectrumViewerEditor.*` | Parameters, acquisition lifecycle, worker scheduling, and UI controls |
| Input transport | `SampleBlockFifo.h`, `SampleWindowAssembler.h`, `BacklogSheddingPolicy.h` | Complete-block SPSC transport, continuous history, exact window cadence, and overload policy |
| Runtime preparation | `AsyncSpectrumAnalysis.*`, `SpectrumAnalysis.*` | Immutable configuration, asynchronous construction, and the worker pipeline |
| Spectral estimation | `DpssTapers.*`, `MultitaperPeriodogram.*`, `SpectrumEstimation.h` | DPSS generation, detrending, tapering, batched FFTs, and calibrated one-sided PSDs |
| Numerical support | `Numerics/SelectedTridiagonalEigensolver.*` | Plugin-private LAPACKE wrapper used only to generate selected DPSS eigenpairs |
| Display | `SpectrumDisplayReducer.*`, `AperiodicSpectrumBaseline.*`, `SpectrumAmplitudeRange.*`, `SpectrumFrameFifo.h`, `SpectrumCanvas.*` | Linear/log bin reduction, optional broad-background display, stable dB ranges, frame publication, axes, cursors, and traces |
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

The replacement sequence is:

1. The message thread snapshots the requested profile, stream, channels, units,
   sample rate, and input-block capacity into a new generation.
2. `AsyncSpectrumAnalysis::request()` replaces any older pending request. Its
   thread builds a complete `PreparedSpectrumAnalysis`, including storage,
   DPSS tapers, reducers, and FFTW plans.
3. The analysis worker accepts only the latest completed generation. A failure
   leaves the previous runtime usable; a superseded result is retired.
4. The worker swaps the active runtime, resets warm-up state, then publishes a
   coherent input route. Publishing the generation is the callback's permission
   to enqueue blocks for that runtime.
5. The new runtime becomes `live` after its first complete spectral window.
   Blocks carrying an older generation are rejected rather than joined to new
   history.

Input-route and display-setting snapshots use an even/odd sequence counter.
The writer marks the snapshot odd, changes its fields, and publishes the next
even value with release ordering. A reader accepts the fields only when the
same even value surrounds its relaxed loads. This is a small fixed-field
publication protocol, not a general replacement for a queue.

Frames and captured references retain stream, channel, unit, frequency, and
generation metadata. Keep that identity attached when adding another output
product; labels must describe the data being drawn, not merely the latest UI
selection.

## Frame publication and overload

Input slots contain whole callbacks and preserve all selected channels as one
planar block. A full FIFO rejects the whole block and records its sample range.
After copying an accepted block into private circular history, the worker
releases the slot before detrending, tapering, or FFT work begins.

Live spectral frames are replaceable state. The worker reduces full-resolution
linear PSDs to display-width mean and peak products, and the canvas consumes
only the newest complete frame. If analysis falls behind, the worker retains
the newest eligible window and records sequence gaps and shedding counters.
Capture completion is different: a final frozen result remains pending until a
frame slot is available, so it cannot be silently lost.

Stopping acquisition first prevents new callback entry, waits for callbacks
already in flight, then asks the analysis worker to stop. Expensive runtime
destruction is handed to the configuration thread. Preserve this ordering when
adding state whose lifetime crosses a thread boundary.

## OpenBLAS packaging

DPSS preparation calls two OpenBLAS entry points: `LAPACKE_dstevr` and
`openblas_set_num_threads`. `cmake/OpenBlasPackages.cmake` is the single source
of package filenames, SHA-256 digests, and feedstock provenance.
`cmake/PrepareOpenBlas.cmake` downloads pinned conda-forge packages into the
build-local download cache, verifies them before extraction, and publishes a
content-addressed stage only after all files and licenses are present. A short
process lock prevents concurrent configure jobs from publishing the same stage.

Linux embeds the static archive and hides all archive symbols. macOS combines
the x86_64 and arm64 archives with `lipo` and uses Apple ld's
`-hidden-lopenblas` form. Windows installs a namespaced DLL in the GUI's shared
directory and links a generated import library containing only the two required
symbols. The eigensolver sets OpenBLAS to one thread before its first solve;
unrestricted OpenBLAS workers make background configuration slower and can
interfere with acquisition.

`SPECTRUM_VIEWER_OPENBLAS_ROOT` bypasses all network access. Point it at a stage
with the layout documented in `README.md`. Do not commit `_deps/` contents or
copy binaries into the source tree. Update `THIRD_PARTY_NOTICES.md` and test all
three platform paths when changing the package pins.

## Invariants for changes

- Do not allocate, lock, plan FFTs, notify a thread, or run DSP in
  `SpectrumViewer::process()`.
- Preserve complete-block channel alignment. Reject a block rather than enqueue
  different sample ranges for different channels.
- Keep estimator and transport storage planar: samples are contiguous within a
  channel.
- Keep PSD values linear through estimation, capture, averaging, and display
  reduction. Convert PSD to ASD or decibels only for presentation.
- Keep aperiodic-background fitting separate from spectral estimation. Publish
  the raw PSD beside the fit, and label removed views as dB above background.
- Treat configurations and DPSS banks as immutable after construction.
- Preserve absolute sample positions and reset window history at discontinuities
  or generation changes.
- Keep queue overflow and backlog shedding observable through counters and frame
  metadata.

Run `ctest --test-dir Build --output-on-failure` after behavioral changes. Use
the benchmarks to justify performance-driven complexity, and record target-rig
tail latency rather than relying only on mean timings.
