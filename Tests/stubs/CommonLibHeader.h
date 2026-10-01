/*
    Test-only replacement for the GUI's CommonLibHeader.h.

    The standalone test executable compiles OpenEphysFFTWBatch.cpp into itself,
    so the batch classes are neither imported nor exported and COMMON_LIB must
    expand to nothing. The GUI's header instead resolves it to dllimport under
    OEPLUGIN, which made every use of those classes emit an import reference to
    a symbol that is defined in the same binary (LNK4217/LNK4286).

    It also pulls in JuceHeader.h, which drags the whole of juce_graphics into
    translation units that only need juce_core. juce_Colours.h then defines
    namespace-scope Colour constants, whose out-of-line constructor is not
    compiled into a headless test binary.

    Shadowing the header keeps the tests standalone and honest: they need the
    FFTW batch declarations, not the Open Ephys visualizer surface.
*/

#pragma once

#ifdef COMMON_LIB
#undef COMMON_LIB
#endif
#define COMMON_LIB
