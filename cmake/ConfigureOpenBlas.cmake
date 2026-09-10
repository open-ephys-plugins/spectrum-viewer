set(SPECTRUM_VIEWER_OPENBLAS_ROOT "" CACHE PATH
    "Prepared OpenBLAS root for an offline build")
set(SPECTRUM_VIEWER_OPENBLAS_DOWNLOAD_DIR
    "${CMAKE_BINARY_DIR}/_deps/openblas/downloads" CACHE PATH
    "Cache for verified OpenBLAS packages")
include("${CMAKE_CURRENT_LIST_DIR}/OpenBlasPackages.cmake")

if(SPECTRUM_VIEWER_OPENBLAS_ROOT)
    get_filename_component(_spectrum_viewer_openblas_root
        "${SPECTRUM_VIEWER_OPENBLAS_ROOT}" ABSOLUTE)
else()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
        set(_spectrum_viewer_openblas_identity
            "linux-64-${SV_OPENBLAS_LINUX_SHA256}")
        set(_spectrum_viewer_openblas_platform linux)
    elseif(APPLE)
        set(_spectrum_viewer_openblas_identity
            "macos-universal-${SV_OPENBLAS_MACOS_X86_SHA256}-${SV_OPENBLAS_MACOS_ARM_SHA256}")
        set(_spectrum_viewer_openblas_platform macos)
        if(NOT CMAKE_LIPO)
            find_program(CMAKE_LIPO NAMES lipo)
            if(NOT CMAKE_LIPO)
                message(FATAL_ERROR "Universal macOS staging requires lipo")
            endif()
        endif()
    elseif(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8
           AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "ARM64")
        set(_spectrum_viewer_openblas_identity
            "windows-64-${SV_OPENBLAS_WINDOWS_DEV_SHA256}-${SV_OPENBLAS_WINDOWS_RUNTIME_SHA256}")
        set(_spectrum_viewer_openblas_platform windows)
    else()
        message(FATAL_ERROR
            "Automatic OpenBLAS staging supports Linux x86_64, universal macOS, "
            "and 64-bit MSVC. Set SPECTRUM_VIEWER_OPENBLAS_ROOT to use a "
            "compatible prepared dependency.")
    endif()

    string(SHA256 _spectrum_viewer_openblas_stage_hash
        "spectrum-viewer-openblas-stage-v1-${_spectrum_viewer_openblas_identity}")
    string(SUBSTRING "${_spectrum_viewer_openblas_stage_hash}" 0 16
        _spectrum_viewer_openblas_stage_id)
    set(_spectrum_viewer_openblas_root
        "${CMAKE_BINARY_DIR}/_deps/openblas/stage/${_spectrum_viewer_openblas_stage_id}")

    set(_spectrum_viewer_prepare_command
        "${CMAKE_COMMAND}"
        "-DPLATFORM=${_spectrum_viewer_openblas_platform}"
        "-DSTAGE_DIR=${_spectrum_viewer_openblas_root}"
        "-DDOWNLOAD_DIR=${SPECTRUM_VIEWER_OPENBLAS_DOWNLOAD_DIR}"
        "-P" "${CMAKE_CURRENT_LIST_DIR}/PrepareOpenBlas.cmake")
    if(APPLE)
        list(INSERT _spectrum_viewer_prepare_command 1 "-DLIPO=${CMAKE_LIPO}")
    elseif(MSVC)
        list(INSERT _spectrum_viewer_prepare_command 1 "-DLIB_TOOL=${CMAKE_AR}")
    endif()

    execute_process(
        COMMAND ${_spectrum_viewer_prepare_command}
        RESULT_VARIABLE _spectrum_viewer_openblas_prepare_result)
    if(NOT _spectrum_viewer_openblas_prepare_result EQUAL 0)
        message(FATAL_ERROR
            "Could not prepare the pinned OpenBLAS dependency (exit "
            "${_spectrum_viewer_openblas_prepare_result})")
    endif()
endif()

if(APPLE)
    set(_spectrum_viewer_openblas_library
        "${_spectrum_viewer_openblas_root}/lib/libopenblas.a")
    if(NOT EXISTS "${_spectrum_viewer_openblas_library}")
        message(FATAL_ERROR
            "SPECTRUM_VIEWER_OPENBLAS_ROOT must contain lib/libopenblas.a")
    endif()

    # Apple ld's hidden-library form prevents thousands of private OpenBLAS
    # symbols from becoming exports of the plugin bundle.
    add_library(SpectrumViewerOpenBLAS INTERFACE)
    target_link_directories(SpectrumViewerOpenBLAS INTERFACE
        "${_spectrum_viewer_openblas_root}/lib")
    target_link_options(SpectrumViewerOpenBLAS INTERFACE
        "LINKER:-hidden-lopenblas")
elseif(MSVC)
    set(_spectrum_viewer_openblas_library
        "${_spectrum_viewer_openblas_root}/lib/spectrum-viewer-openblas.lib")
    set(_spectrum_viewer_openblas_runtime
        "${_spectrum_viewer_openblas_root}/bin/spectrum-viewer-openblas.dll")
    if(NOT EXISTS "${_spectrum_viewer_openblas_library}"
       OR NOT EXISTS "${_spectrum_viewer_openblas_runtime}")
        message(FATAL_ERROR
            "SPECTRUM_VIEWER_OPENBLAS_ROOT must contain the prepared import "
            "library and runtime DLL")
    endif()

    add_library(SpectrumViewerOpenBLAS SHARED IMPORTED GLOBAL)
    set_target_properties(SpectrumViewerOpenBLAS PROPERTIES
        IMPORTED_IMPLIB "${_spectrum_viewer_openblas_library}"
        IMPORTED_LOCATION "${_spectrum_viewer_openblas_runtime}")
else()
    set(_spectrum_viewer_openblas_library
        "${_spectrum_viewer_openblas_root}/lib/libopenblas.a")
    if(NOT EXISTS "${_spectrum_viewer_openblas_library}")
        message(FATAL_ERROR
            "SPECTRUM_VIEWER_OPENBLAS_ROOT must contain lib/libopenblas.a")
    endif()

    add_library(SpectrumViewerOpenBLAS STATIC IMPORTED GLOBAL)
    set_target_properties(SpectrumViewerOpenBLAS PROPERTIES
        IMPORTED_LOCATION "${_spectrum_viewer_openblas_library}")
endif()

add_library(SpectrumViewer::OpenBLAS ALIAS SpectrumViewerOpenBLAS)
target_compile_definitions(SpectrumViewerOpenBLAS INTERFACE
    SPECTRUM_VIEWER_OPENBLAS_VERSION="${SV_OPENBLAS_VERSION}")
message(STATUS "Spectrum Viewer OpenBLAS: ${_spectrum_viewer_openblas_root}")

unset(_spectrum_viewer_openblas_identity)
unset(_spectrum_viewer_openblas_library)
unset(_spectrum_viewer_openblas_platform)
unset(_spectrum_viewer_openblas_prepare_result)
unset(_spectrum_viewer_openblas_root)
unset(_spectrum_viewer_openblas_runtime)
unset(_spectrum_viewer_openblas_stage_hash)
unset(_spectrum_viewer_openblas_stage_id)
unset(_spectrum_viewer_prepare_command)
