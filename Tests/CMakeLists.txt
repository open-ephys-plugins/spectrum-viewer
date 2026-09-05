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
	AtomicSynchronizerTests.cpp)

target_compile_features(spectrum_viewer_tests PRIVATE cxx_std_17)
target_include_directories(
	spectrum_viewer_tests
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Source)
target_link_libraries(spectrum_viewer_tests PRIVATE GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(spectrum_viewer_tests)
