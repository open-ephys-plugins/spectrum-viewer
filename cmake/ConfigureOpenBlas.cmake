include(FetchContent)

set(SPECTRUM_VIEWER_OPENBLAS_SOURCE_DIR "" CACHE PATH
    "Existing OpenBLAS source tree; avoids downloading the pinned source")
option(SPECTRUM_VIEWER_FETCH_OPENBLAS
    "Download the pinned private OpenBLAS backend" ON)

# OpenBLAS is a private implementation detail. Preserve top-level options whose
# generic names are also used by OpenBLAS.
set(_spectrum_viewer_build_benchmarks "${BUILD_BENCHMARKS}")
set(_spectrum_viewer_build_testing_defined FALSE)
if(DEFINED BUILD_TESTING)
    set(_spectrum_viewer_build_testing_defined TRUE)
    set(_spectrum_viewer_build_testing "${BUILD_TESTING}")
endif()
set(_spectrum_viewer_build_shared_libs_defined FALSE)
if(DEFINED BUILD_SHARED_LIBS)
    set(_spectrum_viewer_build_shared_libs_defined TRUE)
    set(_spectrum_viewer_build_shared_libs "${BUILD_SHARED_LIBS}")
endif()
set(_spectrum_viewer_pic "${CMAKE_POSITION_INDEPENDENT_CODE}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_WITHOUT_CBLAS ON CACHE BOOL "" FORCE)
set(BUILD_WITHOUT_LAPACK OFF CACHE BOOL "" FORCE)
set(BUILD_WITHOUT_LAPACKE OFF CACHE BOOL "" FORCE)
set(BUILD_SINGLE OFF CACHE BOOL "" FORCE)
set(BUILD_DOUBLE ON CACHE BOOL "" FORCE)
set(BUILD_COMPLEX OFF CACHE BOOL "" FORCE)
set(BUILD_COMPLEX16 OFF CACHE BOOL "" FORCE)
set(BUILD_LAPACK_DEPRECATED OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(C_LAPACK ON CACHE BOOL "" FORCE)
set(NOFORTRAN ON CACHE BOOL "" FORCE)
set(DYNAMIC_ARCH OFF CACHE BOOL "" FORCE)
set(TARGET GENERIC CACHE STRING "" FORCE)
set(USE_THREAD OFF CACHE BOOL "" FORCE)
set(USE_LOCKING ON CACHE BOOL "" FORCE)
set(NO_AFFINITY ON CACHE BOOL "" FORCE)
set(NO_WARMUP ON CACHE BOOL "" FORCE)
set(INTERFACE64 OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(SPECTRUM_VIEWER_OPENBLAS_SOURCE_DIR)
    add_subdirectory(
        "${SPECTRUM_VIEWER_OPENBLAS_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/openblas"
        EXCLUDE_FROM_ALL)
elseif(SPECTRUM_VIEWER_FETCH_OPENBLAS)
    FetchContent_Declare(
        spectrum_viewer_openblas
        GIT_REPOSITORY https://github.com/OpenMathLib/OpenBLAS.git
        GIT_TAG e0166008be8e466242aa76b2ff75ce3f0fbf574a
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(spectrum_viewer_openblas)
else()
    message(FATAL_ERROR
        "Set SPECTRUM_VIEWER_OPENBLAS_SOURCE_DIR or enable "
        "SPECTRUM_VIEWER_FETCH_OPENBLAS")
endif()

if(NOT TARGET openblas)
    message(FATAL_ERROR "The configured OpenBLAS source did not define target 'openblas'")
endif()

set(BUILD_BENCHMARKS "${_spectrum_viewer_build_benchmarks}" CACHE BOOL
    "Build Spectrum Viewer performance benchmarks" FORCE)
if(_spectrum_viewer_build_testing_defined)
    set(BUILD_TESTING "${_spectrum_viewer_build_testing}" CACHE BOOL "" FORCE)
else()
    unset(BUILD_TESTING CACHE)
endif()
if(_spectrum_viewer_build_shared_libs_defined)
    set(BUILD_SHARED_LIBS "${_spectrum_viewer_build_shared_libs}" CACHE BOOL "" FORCE)
else()
    unset(BUILD_SHARED_LIBS CACHE)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE "${_spectrum_viewer_pic}")

unset(_spectrum_viewer_build_benchmarks)
unset(_spectrum_viewer_build_testing)
unset(_spectrum_viewer_build_testing_defined)
unset(_spectrum_viewer_build_shared_libs)
unset(_spectrum_viewer_build_shared_libs_defined)
unset(_spectrum_viewer_pic)
