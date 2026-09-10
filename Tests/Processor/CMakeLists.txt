set(GUI_TEST_BUILD_DIR
    "${GUI_BASE_DIR}/BuildTests"
    CACHE PATH "Open Ephys GUI build directory configured with BUILD_TESTS=ON")

set(GUI_TEST_LIBRARY_HINTS
    "${GUI_TEST_BUILD_DIR}/TestBin/common"
    "${GUI_TEST_BUILD_DIR}/TestBin/common/Release"
    "${GUI_TEST_BUILD_DIR}/TestBin/common/Debug"
    "${GUI_TEST_BUILD_DIR}/Release"
    "${GUI_TEST_BUILD_DIR}/Debug"
    "${GUI_TEST_BUILD_DIR}/Tests/TestHelpers/Release"
    "${GUI_TEST_BUILD_DIR}/Tests/TestHelpers/Debug")

if(WIN32)
    find_library(GUI_TESTABLE_SOURCE_LIBRARY NAMES gui_testable_source
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
    find_library(GUI_TEST_HELPERS_LIBRARY NAMES test_helpers
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
elseif(APPLE)
    find_file(GUI_TESTABLE_SOURCE_LIBRARY
        NAMES gui_testable_source.dylib libgui_testable_source.dylib
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
    find_file(GUI_TEST_HELPERS_LIBRARY
        NAMES test_helpers.dylib libtest_helpers.dylib
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
else()
    find_file(GUI_TESTABLE_SOURCE_LIBRARY
        NAMES gui_testable_source.so libgui_testable_source.so
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
    find_file(GUI_TEST_HELPERS_LIBRARY
        NAMES test_helpers.so libtest_helpers.so
        HINTS ${GUI_TEST_LIBRARY_HINTS} NO_DEFAULT_PATH)
endif()

if(NOT GUI_TESTABLE_SOURCE_LIBRARY OR NOT GUI_TEST_HELPERS_LIBRARY)
    message(FATAL_ERROR
        "Processor tests require gui_testable_source and test_helpers. "
        "Configure plugin-GUI with BUILD_TESTS=ON and build those two targets, "
        "then set GUI_TEST_BUILD_DIR to that build directory.")
endif()

add_executable(spectrum_viewer_processor_tests
    SpectrumCanvasTests.cpp
    SpectrumViewerLifecycleTests.cpp)
target_compile_features(spectrum_viewer_processor_tests PRIVATE cxx_std_17)
target_compile_definitions(spectrum_viewer_processor_tests PRIVATE TEST_RUNNER=1 BUILD_TESTS=1)
target_include_directories(spectrum_viewer_processor_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Source
    ${GUI_BASE_DIR}/Tests/TestHelpers/include
    ${GUI_BASE_DIR}/Source
    ${GUI_BASE_DIR}/JuceLibraryCode
    ${GUI_BASE_DIR}/JuceLibraryCode/modules
    ${GUI_BASE_DIR}/Plugins/Headers)
target_link_libraries(spectrum_viewer_processor_tests PRIVATE
    ${PLUGIN_NAME}
    gtest_main
    ${GUI_TEST_HELPERS_LIBRARY}
    ${GUI_TESTABLE_SOURCE_LIBRARY})

if(MSVC)
    # Test builds resolve the GUI API from the testable host library, matching
    # Open Ephys' in-tree plugin test convention.
    target_link_libraries(${PLUGIN_NAME} PRIVATE ${GUI_TESTABLE_SOURCE_LIBRARY})
endif()

set_target_properties(spectrum_viewer_processor_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/TestBin/SpectrumViewer")

add_custom_command(TARGET spectrum_viewer_processor_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PLUGIN_NAME}>
        $<TARGET_FILE_DIR:spectrum_viewer_processor_tests>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${GUI_TEST_BUILD_DIR}/TestBin/common"
        $<TARGET_FILE_DIR:spectrum_viewer_processor_tests>)

if(MSVC)
    add_custom_command(TARGET spectrum_viewer_processor_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SpectrumViewerOpenBLAS>
            $<TARGET_FILE_DIR:spectrum_viewer_processor_tests>)
endif()

if(LINUX)
    set_target_properties(spectrum_viewer_processor_tests PROPERTIES BUILD_RPATH "$ORIGIN")
endif()

add_test(NAME spectrum_viewer_processor_tests COMMAND spectrum_viewer_processor_tests)
