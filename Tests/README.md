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

`SampleWindowAssembler` is worker-owned history that converts arbitrary callback blocks into exact overlapping windows and resets on a sample-index discontinuity. It is not connected to the plugin processor yet. The eventual audio-to-worker handoff should use JUCE's `AbstractFifo` with preallocated planar slots.

Pure transport and DSP components belong in this fast standalone suite. Processor-level behavior should use the GUI's `ProcessorTester` and fake-source conventions once the processor integration is ready, rather than mocking Open Ephys lifecycle and parameter behavior locally.
