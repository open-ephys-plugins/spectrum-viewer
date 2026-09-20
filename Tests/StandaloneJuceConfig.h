#pragma once

/*
    The headless test executable compiles juce_core directly. The plugin's
    directory-wide Windows configuration normally imports JUCE from the Open
    Ephys host, so override it before any JUCE header is parsed in each test
    translation unit.
*/
#if defined (_WIN32)
#ifdef JUCE_API
#undef JUCE_API
#endif
#define JUCE_API
#endif
