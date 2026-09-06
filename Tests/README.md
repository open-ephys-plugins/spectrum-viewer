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
performing any FFT work.

`SampleBlockFifo` wraps `juce::AbstractFifo` with preallocated planar block
storage. Its tests cover Spectrum Viewer policy: complete-block overflow,
metadata and channel layout, wraparound, rejection, and quiescent reset. JUCE's
own concurrent tests cover the FIFO primitive's SPSC publication semantics.

`SpectrumFrameFifo` publishes all selected channels as one planar display frame.
Its tests cover coherent metadata and channel data, overflow, slot wraparound,
concurrent publication, and draining stale frames to the newest complete frame.

`ReferencePeriodogram` is an intentionally slow, double-precision direct DFT
used only as a correctness oracle. Its tests pin one-sided PSD calibration,
DC/Nyquist treatment, frequency coordinates, detrending, spectral leakage,
Parseval energy, white-noise density, and planar channel isolation. The live
float FFT implementation must be compared against it rather than replacing it.

Pure transport and DSP components belong in this fast standalone suite. Processor-level behavior should use the GUI's `ProcessorTester` and fake-source conventions once the processor integration is ready, rather than mocking Open Ephys lifecycle and parameter behavior locally.
