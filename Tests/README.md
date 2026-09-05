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

GoogleTest is fetched at configure time and pinned to the version used by `plugin-GUI`. Keep test inputs deterministic. Transport tests should partition identical sample sequences into different callback sizes and compare the resulting sample-indexed windows exactly.
