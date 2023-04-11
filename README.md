# Spectrum Viewer [![DOI](https://zenodo.org/badge/228703189.svg)](https://zenodo.org/badge/latestdoi/228703189)

![Spectrum Viewer Editor](https://open-ephys.github.io/gui-docs/_images/spectrumviewer-01.png)

Displays real-time power spectra for up to 8 continuous channels within the Open Ephys GUI. This design of this plugin is based on the Coherence & Spectrogram Viewer from the Translational NeuroEngineering Laboratory at the University of Minnesota.


## Installation

The Spectrum Viewer plugin is not included by default in the Open Ephys GUI. To install, use ctrl-P to access the Plugin Installer, browse to the "Spectrum Viewer" plugin, and click the "Install" button.

## Usage

![Example screenshot](https://open-ephys.github.io/gui-docs/_images/spectrumviewer-02.png)

Instructions for using the Spectrum Viewer plugin are available [here](https://open-ephys.github.io/gui-docs/User-Manual/Plugins/Spectrum-Viewer.html).

## Building from source

First, follow the instructions on [this page](https://open-ephys.github.io/gui-docs/Developer-Guide/Compiling-the-GUI.html) to build the Open Ephys GUI.

**Important:** This plugin is intended for use with the latest version of the GUI (0.6.0 and higher). The GUI should be compiled from the [`main`](https://github.com/open-ephys/plugin-gui/tree/main) branch, rather than the former `master` branch.

This plugin depends on the `main` branch of the [OpenEphysFFTW](https://github.com/open-ephys-plugins/OpenEphysFFTW/tree/main) library, which must be built and installed first.

Be sure to the `OpenEphysFFTW` and `spectrum-viewer` repositories into a directory at the same level as the `plugin-GUI`, e.g.:
 
```
Code
├── plugin-GUI
│   ├── Build
│   ├── Source
│   └── ...
├── OEPlugins
│   └── spectrum-viewer
│   │   ├── Build
│   │   ├── Source
│   │   └── ...
│   └── OpenEphysFFTW
│       ├── Build
│       ├── Source
│       └── ...
```

### Windows

**Requirements:** [Visual Studio](https://visualstudio.microsoft.com/) and [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Visual Studio 17 2022" -A x64 ..
```

Next, launch Visual Studio and open the `OE_PLUGIN_spectrum-viewer.sln` file that was just created. Select the appropriate configuration (Debug/Release) and build the solution.

Selecting the `INSTALL` project and manually building it will copy the `.dll` and any other required files into the GUI's `plugins` directory. The next time you launch the GUI from Visual Studio, the Spectrum Viewer plugin should be available.


### Linux

**Requirements:** [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Unix Makefiles" ..
cd Debug
make -j
make install
```

This will build the plugin and copy the `.so` file into the GUI's `plugins` directory. The next time you launch the GUI compiled version of the GUI, the Spectrum Viewer plugin should be available.


### macOS

**Requirements:** [Xcode](https://developer.apple.com/xcode/) and [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Xcode" ..
```

Next, launch Xcode and open the `spectrum-viewer.xcodeproj` file that now lives in the “Build” directory.

Running the `ALL_BUILD` scheme will compile the plugin; running the `INSTALL` scheme will install the `.bundle` file to `/Users/<username>/Library/Application Support/open-ephys/plugins-api8`. The Spectrum Viewer plugin should be available the next time you launch the GUI from Xcode.


