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
| Numerical support | `Numerics/SelectedTridiagonalEigensolver.*` | Plugin-private Sturm bisection and inverse iteration, used only to generate selected DPSS eigenpairs |
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

Capture is different in both directions. Backlog shedding is suspended while a
capture runtime is active: a shed capture window is two seconds of data the
average will never see, so discarding one does not help the worker catch up, it
stalls the capture. The only back-pressure for a capture is the input queue, and
an input block dropped there resets window history, which costs a whole window.
Size `INPUT_QUEUE_CAPACITY` to absorb a complete estimate, reduce and baseline
burst rather than trimming it. Capture completion is likewise not replaceable: a
final frozen result remains pending until a frame slot is available, so it
cannot be silently lost.

A capture reports `frozen` only once the worker owns the `CapturedSpectrum` it
would hand to the reference. If the result cannot be retained the state is
`failed` instead, because `frozen` is what enables the reference controls.
`hasRetainedCapture()` is the gate the UI uses, and
`setCurrentCaptureAsReference()` names the retained capture rather than the last
requested one.

A capture's sample span is anchored on the windows it included, never on
arriving blocks. `CapturedSpectrum` rejects a span that does not run forward,
and source sample numbering is not guaranteed to run forward across a
discontinuity, so an anchor taken from a block can outlive the numbering it came
from and discard an otherwise complete capture.

Stopping acquisition first prevents new callback entry, waits for callbacks
already in flight, then asks the analysis worker to stop. Expensive runtime
destruction is handed to the configuration thread. Preserve this ordering when
adding state whose lifetime crosses a thread boundary.

## DPSS eigensolver

`Numerics/SelectedTridiagonalEigensolver.*` finds the K algebraically largest
eigenpairs of a real symmetric tridiagonal matrix: Sturm-sequence bisection for
the eigenvalues, then inverse iteration through a shifted tridiagonal LU for the
vectors. It is written from the published algorithms - Barth, Martin and
Wilkinson (1967) for the bisection; Peters and Wilkinson (1979) and Golub and
Van Loan sections 8.2 and 8.4 for the inverse iteration - and deliberately not
transliterated from LAPACK. The plugin has no BLAS or LAPACK dependency.

`DpssTapers` is the only caller. It reads the eigenvectors; concentration ratios
come from its own FFT, so eigenvalue accuracy matters only as the inverse
iteration shift.

The clustering hazard here is about the *relative* gap, not the absolute one.
The Slepian tridiagonal matrix has absolute eigenvalue gaps of order 1 that do
not shrink with N, so bisection isolates each wanted eigenvalue comfortably. Its
relative gaps, however, fall to roughly 1e-9 at N = 60000, far inside LAPACK's
1e-3 cluster threshold. Independent inverse iterations would therefore return
vectors orders of magnitude less orthogonal than the tests require. All wanted
vectors are treated as a single cluster and reorthogonalized unconditionally,
both inside each iteration and once more at the end. At K <= 8 that costs under
a millisecond against a bisection of tens of milliseconds, so there is no reason
to make it conditional.

Three details are not negotiable, and each one fails silently if changed:

- In the Sturm recurrence the pivot clamp must precede the sign test, and the
  test must be inclusive. Otherwise the count is wrong whenever a pivot is
  exactly zero. The clamps are also what let the solver handle zero
  off-diagonals, exact degeneracy and diagonal matrices with no explicit
  block-splitting code.
- The shifted solve must not use a general banded factorization. `T - theta*I`
  is deliberately near-singular, which is what makes inverse iteration converge;
  a vanishing pivot is clamped rather than reported as an error.
- The start vector must be deterministic and reentrant. The solver is called
  concurrently, so no global or function-local static state, and no
  `std::uniform_real_distribution`, whose output is implementation-defined and
  would make tapers differ between standard libraries.
- The stall test must compare the current residual against the *previous*
  iterate's, not against the running best. An improving iteration has just
  assigned the running best that same value, so comparing with it compares a
  value against itself, stops refinement after one solve on every matrix, and
  leaves `solverInfo` unable to report anything. No test can catch this: the
  mistake is the variable passed at the call site, and how many solves a pair
  needs is platform-dependent, so a correct solver reaches the residual floor
  in two solves on some targets and later on others.

A success status means the solve ran, not that the vectors are usable. Each
eigenpair whose residual stalls above 1e-13 is counted in `solverInfo`, and
`largestResidual` carries the value that judgement was made on. `DpssTapers`
requires both `succeeded()` and `solverInfo == 0`; treating the status alone as
the contract silently accepts unconverged tapers.

Accuracy targets, checked by `Tests/SelectedTridiagonalEigensolverTests.cpp`:
normalized residual below 1e-13 and orthonormality below 5e-12 at the
production-scale N = 60000 case. `Tests/DpssTapersTests.cpp` is the end-to-end
criterion, including SciPy golden values.

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
- Shed only live display frames. Never shed, and never silently drop, a window
  that a capture is accumulating.
- Do not advertise a state the user can act on unless the data behind it exists.
  A frozen capture must be a retained capture.
- Keep numerical dependencies in-tree. Do not reintroduce BLAS or LAPACK; the CI
  `ldd` and `otool` checks enforce this.

Run `ctest --test-dir Build --output-on-failure` after behavioral changes. Use
the benchmarks to justify performance-driven complexity, and record target-rig
tail latency rather than relying only on mean timings.
