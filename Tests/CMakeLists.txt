include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG release-1.12.1
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(
	spectrum_viewer_tests
	SampleWindowAssemblerTests.cpp)

target_compile_features(spectrum_viewer_tests PRIVATE cxx_std_17)
if(MSVC)
	target_compile_options(spectrum_viewer_tests PRIVATE /W4)
else()
	target_compile_options(spectrum_viewer_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
target_include_directories(
	spectrum_viewer_tests
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Source)
target_link_libraries(spectrum_viewer_tests PRIVATE gtest_main)

add_test(NAME spectrum_viewer_tests COMMAND spectrum_viewer_tests)
