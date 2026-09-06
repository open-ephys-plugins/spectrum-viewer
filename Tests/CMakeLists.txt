include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	googletest
	GIT_REPOSITORY https://github.com/google/googletest.git
	GIT_TAG release-1.12.1
	GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

set(OPENEPHYS_FFTW_SOURCE_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysFFTW"
	CACHE PATH "Path to the matching OpenEphysFFTW source checkout")
if(NOT EXISTS "${OPENEPHYS_FFTW_SOURCE_DIR}/Source/OpenEphysFFTWBatch.cpp")
	message(FATAL_ERROR "Tests require the matching OpenEphysFFTW batch source checkout")
endif()

set(OPENEPHYS_NUMERICS_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../OpenEphysNumerics"
    CACHE PATH "Path to the matching OpenEphysNumerics source checkout")
if(NOT EXISTS "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Source/OpenEphysNumerics.h")
    message(FATAL_ERROR "Tests require the matching OpenEphysNumerics source checkout")
endif()
find_library(TEST_OPENEPHYS_NUMERICS_LIBRARY NAMES OpenEphysNumerics
    HINTS "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Build"
          "${OPENEPHYS_NUMERICS_SOURCE_DIR}/Build/Release"
          "${GUI_COMMONLIB_DIR}/Release/lib/x64")
if(NOT TEST_OPENEPHYS_NUMERICS_LIBRARY)
    message(FATAL_ERROR
        "Tests require a built OpenEphysNumerics library; build that sibling repository first")
endif()

find_library(TEST_FFTW_DOUBLE_LIBRARY NAMES libfftw3-3 fftw3 fftw3-3
	HINTS ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/linux/lib
	      ${OPENEPHYS_FFTW_SOURCE_DIR}/libs/macos/lib)
find_library(TEST_FFTW_FLOAT_LIBRARY NAMES libfftw3f-3 fftw3f fftw3f-3)
if(UNIX AND NOT TEST_FFTW_FLOAT_LIBRARY)
	find_file(TEST_FFTW_FLOAT_LIBRARY NAMES libfftw3f.so.3
		PATHS /usr/lib /usr/lib64 /lib /lib64)
endif()
if(NOT TEST_FFTW_DOUBLE_LIBRARY OR NOT TEST_FFTW_FLOAT_LIBRARY)
	message(FATAL_ERROR "Tests require float and double FFTW libraries")
endif()

add_executable(
	spectrum_viewer_tests
	${OPENEPHYS_FFTW_SOURCE_DIR}/Source/OpenEphysFFTWBatch.cpp
	../Source/DpssTapers.cpp
	../Source/MultitaperPeriodogram.cpp
	../Source/ReferencePeriodogram.cpp
	../Source/SingleTaperPeriodogram.cpp
	JuceAbstractFifo.cpp
	OpenEphysFFTWBatchTests.cpp
	DpssTapersTests.cpp
	MultitaperPeriodogramTests.cpp
	ReferencePeriodogramTests.cpp
	SampleBlockFifoTests.cpp
	SampleWindowAssemblerTests.cpp
	SingleTaperPeriodogramTests.cpp
	SpectrumFrameFifoTests.cpp)

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
		${OPENEPHYS_NUMERICS_SOURCE_DIR}/Source
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
	${TEST_OPENEPHYS_NUMERICS_LIBRARY}
	${CMAKE_DL_LIBS})

if(LINUX)
	target_link_libraries(spectrum_viewer_tests PRIVATE pthread rt)
endif()

add_test(NAME spectrum_viewer_tests COMMAND spectrum_viewer_tests)
