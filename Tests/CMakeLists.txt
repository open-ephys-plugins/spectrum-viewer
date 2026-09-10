include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG release-1.12.1
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

if(APPLE)
	enable_language(OBJCXX)
	set_source_files_properties(JuceCore.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

set(OPENEPHYS_FFTW_SOURCE_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW"
	CACHE PATH "Path to the matching OpenEphysFFTW source checkout")
if(NOT EXISTS "${OPENEPHYS_FFTW_SOURCE_DIR}/Source/OpenEphysFFTWBatch.cpp")
	message(FATAL_ERROR "Tests require the matching OpenEphysFFTW batch source checkout")
endif()

find_library(TEST_FFTW_DOUBLE_LIBRARY NAMES libfftw3 libfftw3-3 fftw3 fftw3-3
	HINTS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/linux/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/macos/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/windows/lib/x64
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/windows/lib/x64)
find_library(TEST_FFTW_FLOAT_LIBRARY NAMES libfftw3f libfftw3f-3 fftw3f fftw3f-3
	HINTS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/linux/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/macos/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/windows/lib/x64
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/windows/lib/x64)
if(UNIX AND NOT TEST_FFTW_FLOAT_LIBRARY)
	find_file(TEST_FFTW_FLOAT_LIBRARY NAMES libfftw3f.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
endif()
if(NOT TEST_FFTW_DOUBLE_LIBRARY OR NOT TEST_FFTW_FLOAT_LIBRARY)
	message(FATAL_ERROR "Tests require float and double FFTW libraries")
endif()

if(LINUX)
	find_file(TEST_FFTW_DOUBLE_RUNTIME NAMES libfftw3.so.3
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/linux/bin
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/bin
		NO_DEFAULT_PATH)
	find_file(TEST_FFTW_FLOAT_RUNTIME NAMES libfftw3f.so.3
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/linux/bin
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/bin
		NO_DEFAULT_PATH)
elseif(APPLE)
	find_file(TEST_FFTW_DOUBLE_RUNTIME NAMES libfftw3.3.dylib
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/macos/bin
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/bin
		NO_DEFAULT_PATH)
	find_file(TEST_FFTW_FLOAT_RUNTIME NAMES libfftw3f.3.dylib
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/macos/bin
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/bin
		NO_DEFAULT_PATH)
elseif(MSVC)
	find_file(TEST_FFTW_DOUBLE_RUNTIME NAMES fftw3.dll libfftw3-3.dll
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/windows/bin/x64
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/windows/bin/x64
		NO_DEFAULT_PATH)
	find_file(TEST_FFTW_FLOAT_RUNTIME NAMES fftw3f.dll libfftw3f-3.dll
		PATHS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/windows/bin/x64
		      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/windows/bin/x64
		NO_DEFAULT_PATH)
endif()
if(NOT TEST_FFTW_DOUBLE_RUNTIME OR NOT TEST_FFTW_FLOAT_RUNTIME)
	message(FATAL_ERROR "Tests require staged float and double FFTW runtimes")
endif()

add_executable(
	spectrum_viewer_tests
	${OPENEPHYS_FFTW_SOURCE_DIR}/Source/OpenEphysFFTWBatch.cpp
	../Source/AperiodicSpectrumBaseline.cpp
	../Source/DpssTapers.cpp
	../Source/Numerics/SelectedTridiagonalEigensolver.cpp
	../Source/AsyncSpectrumAnalysis.cpp
	../Source/MultitaperPeriodogram.cpp
	../Source/ReferencePeriodogram.cpp
	../Source/SingleTaperPeriodogram.cpp
	../Source/SpectrumAnalysis.cpp
	../Source/SpectrumAmplitudeRange.cpp
	../Source/SpectrumCaptureAccumulator.cpp
	../Source/SpectrumDisplayReducer.cpp
	../Source/SpectrumReference.cpp
	AperiodicSpectrumBaselineTests.cpp
	AsyncSpectrumAnalysisTests.cpp
	BacklogSheddingPolicyTests.cpp
	JuceCore.cpp
	OpenEphysFFTWBatchTests.cpp
	DpssTapersTests.cpp
	SelectedTridiagonalEigensolverTests.cpp
	MultitaperPeriodogramTests.cpp
	ReferencePeriodogramTests.cpp
	SampleBlockFifoTests.cpp
	SampleWindowAssemblerTests.cpp
	SingleTaperPeriodogramTests.cpp
	SpectrumAnalysisTests.cpp
	SpectrumAmplitudeRangeTests.cpp
	SpectrumCaptureAccumulatorTests.cpp
	SpectrumDisplayReducerTests.cpp
	SpectrumFrameFifoTests.cpp
	SpectrumReferenceTests.cpp)

target_compile_features(spectrum_viewer_tests PRIVATE cxx_std_17)
if(MSVC)
	target_compile_options(spectrum_viewer_tests PRIVATE /W4)
else()
	target_compile_options(spectrum_viewer_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()
target_include_directories(
	spectrum_viewer_tests
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/../Source
		${OPENEPHYS_FFTW_SOURCE_DIR}/Source
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/linux/include
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/macos/include
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/generated/windows/include
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/include
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/include
		${OPENEPHYS_FFTW_SOURCE_DIR}/libs/windows/include
		${GUI_COMMONLIB_DIR}/include
		${GUI_BASE_DIR}/JuceLibraryCode
		${GUI_BASE_DIR}/JuceLibraryCode/modules
		${GUI_BASE_DIR}/Plugins/Headers)
target_link_libraries(spectrum_viewer_tests PRIVATE
	gtest_main
	${TEST_FFTW_DOUBLE_LIBRARY}
	${TEST_FFTW_FLOAT_LIBRARY}
	SpectrumViewer::OpenBLAS
	${CMAKE_DL_LIBS})

if(LINUX)
	target_link_libraries(spectrum_viewer_tests PRIVATE pthread rt)
	set_target_properties(spectrum_viewer_tests PROPERTIES BUILD_RPATH "$ORIGIN")
elseif(APPLE)
	target_link_libraries(spectrum_viewer_tests PRIVATE
		"-framework Cocoa"
		"-framework Foundation"
		"-framework IOKit"
		"-framework Security")
	set_target_properties(spectrum_viewer_tests PROPERTIES BUILD_RPATH "@loader_path")
endif()

add_custom_command(TARGET spectrum_viewer_tests POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${TEST_FFTW_DOUBLE_RUNTIME}
		$<TARGET_FILE_DIR:spectrum_viewer_tests>
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		${TEST_FFTW_FLOAT_RUNTIME}
		$<TARGET_FILE_DIR:spectrum_viewer_tests>)

if(MSVC)
	add_custom_command(TARGET spectrum_viewer_tests POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			$<TARGET_FILE:SpectrumViewerOpenBLAS>
			$<TARGET_FILE_DIR:spectrum_viewer_tests>)
endif()

add_test(NAME spectrum_viewer_tests COMMAND spectrum_viewer_tests)
