include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	googlebenchmark
	GIT_REPOSITORY https://github.com/google/benchmark.git
	GIT_TAG v1.8.3
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(
	spectrum_viewer_benchmarks
	AmplitudeRangeBenchmarks.cpp
	CaptureAccumulatorBenchmarks.cpp
	ReferenceComparisonBenchmarks.cpp
	WindowPreprocessingBenchmarks.cpp
	../Source/SpectrumAmplitudeRange.cpp
	../Source/SpectrumCaptureAccumulator.cpp
	../Source/SpectrumDisplayReducer.cpp
	../Source/SpectrumReference.cpp)

target_compile_features(spectrum_viewer_benchmarks PRIVATE cxx_std_17)

if(MSVC)
	target_compile_options(spectrum_viewer_benchmarks PRIVATE /O2 /W4)
	if(BENCHMARK_NATIVE_ARCH)
		target_compile_options(spectrum_viewer_benchmarks PRIVATE /arch:AVX2)
	endif()
else()
	target_compile_options(spectrum_viewer_benchmarks PRIVATE -O3 -Wall -Wextra -Wpedantic)
	if(BENCHMARK_NATIVE_ARCH)
		target_compile_options(spectrum_viewer_benchmarks PRIVATE -march=native)
	endif()
endif()

target_link_libraries(spectrum_viewer_benchmarks PRIVATE benchmark::benchmark)

if(BUILD_PROCESSOR_TESTS AND GUI_TESTABLE_SOURCE_LIBRARY AND GUI_TEST_HELPERS_LIBRARY)
	target_sources(spectrum_viewer_benchmarks PRIVATE GuiRepaintBenchmarks.cpp)
	target_compile_definitions(spectrum_viewer_benchmarks PRIVATE TEST_RUNNER=1 BUILD_TESTS=1)
	target_include_directories(spectrum_viewer_benchmarks SYSTEM PRIVATE
		${GUI_BASE_DIR}/Tests/TestHelpers/include
		${GUI_BASE_DIR}/Source
		${GUI_BASE_DIR}/JuceLibraryCode
		${GUI_BASE_DIR}/JuceLibraryCode/modules
		${GUI_BASE_DIR}/Plugins/Headers)
	target_link_libraries(spectrum_viewer_benchmarks PRIVATE
		${PLUGIN_NAME}
		${GUI_TEST_HELPERS_LIBRARY}
		${GUI_TESTABLE_SOURCE_LIBRARY})
	if(LINUX)
		set_target_properties(spectrum_viewer_benchmarks PROPERTIES
			BUILD_RPATH "$ORIGIN/../TestBin/SpectrumViewer;$ORIGIN/../../plugin-GUI/BuildTests/TestBin/common")
	endif()
else()
	message(STATUS "Skipping GUI repaint benchmarks: BUILD_PROCESSOR_TESTS and Open Ephys test libraries are required")
endif()

find_path(FFTW_BENCH_INCLUDE_DIR fftw3.h
	HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/${CMAKE_SYSTEM_NAME}/include
	      ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/linux/include
	      ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/macos/include
	      ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/windows/include)
if(UNIX)
	find_file(FFTW_BENCH_SYSTEM_DOUBLE_LIBRARY NAMES libfftw3.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
	find_file(FFTW_BENCH_SYSTEM_FLOAT_LIBRARY NAMES libfftw3f.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
	find_file(FFTW_BENCH_DOUBLE_THREADS_LIBRARY NAMES libfftw3_threads.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
	find_file(FFTW_BENCH_FLOAT_THREADS_LIBRARY NAMES libfftw3f_threads.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
endif()
if(FFTW_BENCH_SYSTEM_DOUBLE_LIBRARY AND FFTW_BENCH_SYSTEM_FLOAT_LIBRARY)
	set(FFTW_BENCH_DOUBLE_LIBRARY ${FFTW_BENCH_SYSTEM_DOUBLE_LIBRARY})
	set(FFTW_BENCH_FLOAT_LIBRARY ${FFTW_BENCH_SYSTEM_FLOAT_LIBRARY})
else()
	find_library(FFTW_BENCH_DOUBLE_LIBRARY NAMES libfftw3-3 fftw3 fftw3-3
		HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/linux/lib
		      ${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW/libs/macos/lib)
	find_library(FFTW_BENCH_FLOAT_LIBRARY NAMES libfftw3f-3 fftw3f fftw3f-3)
endif()

if(FFTW_BENCH_INCLUDE_DIR AND FFTW_BENCH_DOUBLE_LIBRARY AND FFTW_BENCH_FLOAT_LIBRARY)
	set(OPENEPHYS_FFTW_SOURCE_DIR
		"${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW"
		CACHE PATH "Path to the matching OpenEphysFFTW source checkout")
	target_sources(spectrum_viewer_benchmarks PRIVATE
		${OPENEPHYS_FFTW_SOURCE_DIR}/Source/OpenEphysFFTWBatch.cpp
		../Source/SingleTaperPeriodogram.cpp
		FftwPlanBenchmarks.cpp
		SingleTaperPeriodogramBenchmarks.cpp)
	target_include_directories(spectrum_viewer_benchmarks PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../Source
		${OPENEPHYS_FFTW_SOURCE_DIR}/Source
		${FFTW_BENCH_INCLUDE_DIR})
	target_link_libraries(spectrum_viewer_benchmarks PRIVATE
		${FFTW_BENCH_DOUBLE_LIBRARY}
		${FFTW_BENCH_FLOAT_LIBRARY})

	set(OPENEPHYS_NUMERICS_SOURCE_DIR
		"${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysNumerics"
		CACHE PATH "Path to the matching OpenEphysNumerics source checkout")
	find_library(BENCH_OPENEPHYS_NUMERICS_LIBRARY NAMES OpenEphysNumerics
		HINTS "${GUI_COMMONLIB_DIR}/Release/lib/x64"
		      "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Build"
		      "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Build/Release")
	if(EXISTS "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Source/OpenEphysNumerics.h"
	   AND BENCH_OPENEPHYS_NUMERICS_LIBRARY)
		target_sources(spectrum_viewer_benchmarks PRIVATE
			../Source/DpssTapers.cpp
			../Source/MultitaperPeriodogram.cpp
			../Source/SpectrumAnalysis.cpp
			MultitaperPeriodogramBenchmarks.cpp)
		target_include_directories(spectrum_viewer_benchmarks PRIVATE
			${OPENEPHYS_NUMERICS_SOURCE_DIR}/Source)
		target_link_libraries(spectrum_viewer_benchmarks PRIVATE
			${BENCH_OPENEPHYS_NUMERICS_LIBRARY})
	else()
		message(STATUS "Skipping multitaper benchmark: OpenEphysNumerics is required")
	endif()
	if(FFTW_BENCH_DOUBLE_THREADS_LIBRARY AND FFTW_BENCH_FLOAT_THREADS_LIBRARY)
		target_compile_definitions(spectrum_viewer_benchmarks PRIVATE SPECTRUM_VIEWER_FFTW_THREADS=1)
		target_link_libraries(spectrum_viewer_benchmarks PRIVATE
			${FFTW_BENCH_DOUBLE_THREADS_LIBRARY}
			${FFTW_BENCH_FLOAT_THREADS_LIBRARY})
	endif()
else()
	message(STATUS "Skipping FFTW plan benchmarks: both float and double FFTW libraries are required")
endif()
