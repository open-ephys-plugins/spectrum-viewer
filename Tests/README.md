# Spectrum Viewer Tests

These headless tests exercise DSP and transport components without launching the Open Ephys GUI. Configure the plugin after building the GUI and installing OpenEphysFFTW:

```bash
cmake -S spectrum-viewer -B spectrum-viewer/Build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGUI_BASE_DIR="$PWD/plugin-GUI" \
  -DBUILD_TESTS=ON
cmake --build spectrum-viewer/Build --target spectrum_viewer_tests --parallel
ctest --test-dir spectrum-viewer/Build --output-on-failure
```

GoogleTest is fetched at configure time and pinned to the version used by `plugin-GUI`. CTest runs the suite as one test executable, following the Open Ephys component-test convention. Keep inputs deterministic and test plugin behavior rather than duplicating JUCE's own primitive tests.

`SampleWindowAssembler` is worker-owned history that converts arbitrary callback
blocks into exact overlapping windows and resets on a sample-index
discontinuity. It copies a complete block before exposing ready windows, allowing
the processor to release its preallocated planar `SampleBlockFifo` slot before
performing any FFT work. Its explicit discard operation advances along the same
sample-indexed hop grid without computing obsolete windows.

`BacklogSheddingPolicy` is a queue- and DSP-independent scheduling primitive.
When a consumer observes queued input behind the block it just dequeued, it
discards intermediate completed work, retains only the newest work at catch-up,
and then resumes normal processing. This intentionally small interface is a
candidate for reuse by other real-time Open Ephys plugins.

`SampleBlockFifo` wraps `juce::AbstractFifo` with preallocated planar block
storage. Its tests cover Spectrum Viewer policy: complete-block overflow,
metadata and channel layout, wraparound, rejection, and quiescent reset. JUCE's
own concurrent tests cover the FIFO primitive's SPSC publication semantics.

`SpectrumFrameFifo` publishes all selected channels as one planar display frame.
Its tests cover coherent metadata and channel data, overflow, slot wraparound,
concurrent publication, and draining stale frames to the newest complete frame.
Reduced frames additionally carry native channel units, exact frequency bounds
and coordinates, frequency-axis scale, area-weighted PSD means, and a narrow-line
peak envelope.

`SpectrumDisplayReducer` projects the complete one-sided PSD onto the current
plot width without allocation. Tests pin DC/Nyquist half-bin support,
integrated-power conservation, flat-noise level, narrow-line preservation, and
monotonic linear/log frequency coordinates. Log display excludes DC because
zero has no logarithmic coordinate; the underlying full PSD remains unchanged.

`SpectrumAmplitudeRange` defaults to a fixed -120 to 20 dB display and rejects
fixed spans narrower than 20 dB. Its optional auto mode fits the lower bound
from a robust mean-PSD percentile, retains the strongest peak-envelope value,
and follows targets using signal time rather than repaint cadence. Tests pin
outlier rejection, peak visibility, fixed-range stability, unit resets, and the
faster outward/slower inward response.

`ReferencePeriodogram` is an intentionally slow, double-precision direct DFT
used only as a correctness oracle. Its tests pin one-sided PSD calibration,
DC/Nyquist treatment, frequency coordinates, detrending, spectral leakage,
Parseval energy, white-noise density, and planar channel isolation. The live
float FFT implementation must be compared against it rather than replacing it.

`SingleTaperPeriodogram` is the allocation-free production candidate. It uses
float FFTW storage and accepts each channel as one or two chronological spans,
so wrapped worker history is detrended and tapered directly into aligned FFT
input. Its tests compare every output bin against the double oracle across
odd/even lengths, all detrend modes, planar channels, wrapped input, and a weak
tone beside a strong tone and large offset. The lightweight OpenEphysFFTW batch
implementation is compiled directly into this headless target to avoid loading
the legacy wrapper's GUI/JUCE dependencies.

`MultitaperPeriodogram` applies one immutable DPSS bank with cache-tiled float
preprocessing and a persistent channel-by-taper FFT batch. It estimates each
trend once, normalizes each eigenspectrum by the actual stored taper energy,
and averages only in linear power. Tests compare every bin with the independent
double direct-DFT oracle, including odd/even endpoints, every detrend mode,
wrapped windows, channel isolation, white-noise density, and rejected-frame
output integrity. Line fitting, adaptive weighting, and temporal smoothing are
deliberately outside this class.

`SpectrumAnalysisPipeline` is the worker-side integration boundary. Its
immutable configuration owns a generated DPSS bank, while the pipeline combines
sample-indexed circular history with the persistent multitaper estimator. Tests
pin arbitrary callback partitioning, exact hop/sample metadata, generation
isolation, discontinuity resets, failed-window accounting, and agreement with
the standalone estimator. Published frames contain the complete one-sided PSD,
fixed source-channel mapping, and the configuration needed to interpret every
bin.

`AsyncSpectrumAnalysis` prepares the complete immutable runtime—including the
DPSS bank, history, estimator plan, and output FIFO—on a dedicated
configuration thread. Tests hold a synthetic build in progress to verify that
new requests return immediately, that rapid changes coalesce to the newest
generation, and that a failed build is reported without killing the service.
Superseded and retired runtimes are destroyed by the configuration thread.

Pure transport and DSP components belong in this fast standalone suite.
Processor-level lifecycle and overload behavior uses the GUI's `ProcessorTester`
and fake-source conventions rather than mocking Open Ephys behavior locally.
`SpectrumCanvasTests` additionally renders the real component tree into a JUCE
software image. It verifies full-Nyquist linear display, mean and peak traces,
native PSD/ASD labels, hot logarithmic switching, valid transformed coordinates,
and trace coverage across the plot width. Set `SPECTRUM_VIEWER_TEST_IMAGE_DIR`
to retain PNG artifacts for manual inspection.
