/* Compile the JUCE core facilities used by the standalone headless tests. */
#if defined (_WIN32) && defined (JUCE_API)
#undef JUCE_API
#define JUCE_API
#endif

#include <AppConfig.h>

#define JUCE_USE_CURL 0
#include <juce_core/juce_core.cpp>
#include <juce_core/juce_core_CompilationTime.cpp>
