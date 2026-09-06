/* Compile the JUCE FIFO implementation needed by the standalone headless tests. */
#if defined (_WIN32) && defined (JUCE_API)
#undef JUCE_API
#define JUCE_API
#endif

#include <AppConfig.h>
#include <juce_core/juce_core.h>
#include <juce_core/containers/juce_AbstractFifo.cpp>

namespace juce
{
#if JUCE_DEBUG
this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode::
    this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_debug_mode() noexcept
{
}
#else
this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode::
    this_will_fail_to_link_if_some_of_your_compile_units_are_built_in_release_mode() noexcept
{
}
#endif
} // namespace juce
