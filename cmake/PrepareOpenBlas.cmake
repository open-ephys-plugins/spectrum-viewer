cmake_minimum_required(VERSION 3.15)
include("${CMAKE_CURRENT_LIST_DIR}/OpenBlasPackages.cmake")

foreach(_required PLATFORM STAGE_DIR DOWNLOAD_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "PrepareOpenBlas.cmake requires ${_required}")
    endif()
endforeach()

get_filename_component(STAGE_DIR "${STAGE_DIR}" ABSOLUTE)
get_filename_component(DOWNLOAD_DIR "${DOWNLOAD_DIR}" ABSOLUTE)
get_filename_component(_stage_parent "${STAGE_DIR}" DIRECTORY)
file(MAKE_DIRECTORY "${_stage_parent}" "${DOWNLOAD_DIR}")
file(LOCK "${_stage_parent}/prepare.lock" GUARD PROCESS TIMEOUT 300)

if(EXISTS "${STAGE_DIR}/MANIFEST.txt")
    return()
endif()

if(EXISTS "${STAGE_DIR}")
    message(FATAL_ERROR
        "Incomplete OpenBLAS stage at ${STAGE_DIR}; remove that directory and "
        "configure again")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _work_nonce)
set(_work "${STAGE_DIR}.preparing-${_work_nonce}")
file(MAKE_DIRECTORY "${_work}" "${_work}/stage/lib" "${_work}/stage/licenses")

function(_download_and_extract name url sha256 output_root)
    set(_archive "${DOWNLOAD_DIR}/${name}.conda")
    if(EXISTS "${_archive}")
        file(SHA256 "${_archive}" _existing_sha256)
        if(NOT _existing_sha256 STREQUAL "${sha256}")
            file(REMOVE "${_archive}")
        endif()
    endif()

    if(NOT EXISTS "${_archive}")
        set(_partial "${_archive}.part")
        file(REMOVE "${_partial}")
        message(STATUS "Downloading ${name} from conda-forge")
        file(DOWNLOAD "${url}" "${_partial}"
            EXPECTED_HASH "SHA256=${sha256}"
            TLS_VERIFY ON
            INACTIVITY_TIMEOUT 60
            STATUS _download_status)
        list(GET _download_status 0 _download_code)
        if(NOT _download_code EQUAL 0)
            list(GET _download_status 1 _download_message)
            file(REMOVE "${_partial}")
            message(FATAL_ERROR "Could not download ${name}: ${_download_message}")
        endif()
        file(RENAME "${_partial}" "${_archive}")
    endif()

    set(_package_work "${_work}/${name}")
    file(MAKE_DIRECTORY "${_package_work}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${_archive}"
        WORKING_DIRECTORY "${_package_work}"
        RESULT_VARIABLE _extract_container_result)
    if(NOT _extract_container_result EQUAL 0)
        message(FATAL_ERROR "Could not extract ${_archive}")
    endif()

    file(GLOB _payload_archives "${_package_work}/pkg-*.tar.zst")
    file(GLOB _metadata_archives "${_package_work}/info-*.tar.zst")
    list(LENGTH _payload_archives _payload_count)
    list(LENGTH _metadata_archives _metadata_count)
    if(NOT _payload_count EQUAL 1 OR NOT _metadata_count EQUAL 1)
        message(FATAL_ERROR "${_archive} is not a supported .conda package")
    endif()
    list(GET _payload_archives 0 _payload_archive)
    list(GET _metadata_archives 0 _metadata_archive)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${_payload_archive}"
        WORKING_DIRECTORY "${_package_work}"
        RESULT_VARIABLE _extract_payload_result)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xf "${_metadata_archive}"
        WORKING_DIRECTORY "${_package_work}"
        RESULT_VARIABLE _extract_metadata_result)
    if(NOT _extract_payload_result EQUAL 0 OR NOT _extract_metadata_result EQUAL 0)
        message(FATAL_ERROR "Could not extract the contents of ${_archive}")
    endif()

    set(${output_root} "${_package_work}" PARENT_SCOPE)
endfunction()

if(PLATFORM STREQUAL "linux")
    set(_package_name "${SV_OPENBLAS_LINUX_NAME}")
    set(_package_sha "${SV_OPENBLAS_LINUX_SHA256}")
    _download_and_extract(
        "${_package_name}"
        "${SV_OPENBLAS_BASE_URL}/linux-64/${_package_name}.conda"
        "${_package_sha}"
        _license_root)
    set(_source_library
        "${_license_root}/lib/libopenblasp-r${SV_OPENBLAS_VERSION}.a")
    configure_file("${_source_library}" "${_work}/stage/lib/libopenblas.a" COPYONLY)
    set(_packages "${_package_name}.conda SHA256=${_package_sha}")
elseif(PLATFORM STREQUAL "macos")
    if(NOT DEFINED LIPO OR "${LIPO}" STREQUAL "")
        message(FATAL_ERROR "Universal macOS staging requires LIPO")
    endif()
    set(_x86_name "${SV_OPENBLAS_MACOS_X86_NAME}")
    set(_x86_sha "${SV_OPENBLAS_MACOS_X86_SHA256}")
    set(_arm_name "${SV_OPENBLAS_MACOS_ARM_NAME}")
    set(_arm_sha "${SV_OPENBLAS_MACOS_ARM_SHA256}")
    _download_and_extract(
        "${_x86_name}" "${SV_OPENBLAS_BASE_URL}/osx-64/${_x86_name}.conda"
        "${_x86_sha}" _x86_root)
    _download_and_extract(
        "${_arm_name}" "${SV_OPENBLAS_BASE_URL}/osx-arm64/${_arm_name}.conda"
        "${_arm_sha}" _arm_root)
    set(_license_root "${_x86_root}")
    execute_process(
        COMMAND "${LIPO}" -create
            "${_x86_root}/lib/libopenblasp-r${SV_OPENBLAS_VERSION}.a"
            "${_arm_root}/lib/libopenblas_vortexp-r${SV_OPENBLAS_VERSION}.a"
            -output "${_work}/stage/lib/libopenblas.a"
        RESULT_VARIABLE _lipo_result)
    if(NOT _lipo_result EQUAL 0)
        message(FATAL_ERROR "Could not create the universal OpenBLAS archive")
    endif()
    set(_packages
        "${_x86_name}.conda SHA256=${_x86_sha}\n${_arm_name}.conda SHA256=${_arm_sha}")
elseif(PLATFORM STREQUAL "windows")
    if(NOT DEFINED LIB_TOOL OR "${LIB_TOOL}" STREQUAL "")
        message(FATAL_ERROR "Windows staging requires LIB_TOOL")
    endif()
    set(_dev_name "${SV_OPENBLAS_WINDOWS_DEV_NAME}")
    set(_dev_sha "${SV_OPENBLAS_WINDOWS_DEV_SHA256}")
    set(_runtime_name "${SV_OPENBLAS_WINDOWS_RUNTIME_NAME}")
    set(_runtime_sha "${SV_OPENBLAS_WINDOWS_RUNTIME_SHA256}")
    _download_and_extract(
        "${_dev_name}" "${SV_OPENBLAS_BASE_URL}/win-64/${_dev_name}.conda"
        "${_dev_sha}" _license_root)
    _download_and_extract(
        "${_runtime_name}" "${SV_OPENBLAS_BASE_URL}/win-64/${_runtime_name}.conda"
        "${_runtime_sha}" _runtime_root)
    file(MAKE_DIRECTORY "${_work}/stage/bin")
    configure_file(
        "${_runtime_root}/Library/bin/openblas.dll"
        "${_work}/stage/bin/spectrum-viewer-openblas.dll" COPYONLY)
    set(_definition_file "${_work}/spectrum-viewer-openblas.def")
    file(WRITE "${_definition_file}"
        "LIBRARY spectrum-viewer-openblas.dll\nEXPORTS\n"
        "    LAPACKE_dstevr\n    openblas_set_num_threads\n")
    execute_process(
        COMMAND "${LIB_TOOL}" /nologo "/def:${_definition_file}" /machine:X64
            "/out:${_work}/stage/lib/spectrum-viewer-openblas.lib"
        RESULT_VARIABLE _lib_result)
    if(NOT _lib_result EQUAL 0)
        message(FATAL_ERROR "Could not create the narrow OpenBLAS import library")
    endif()
    set(_packages
        "${_dev_name}.conda SHA256=${_dev_sha}\n${_runtime_name}.conda SHA256=${_runtime_sha}")
else()
    message(FATAL_ERROR "Unsupported OpenBLAS staging platform: ${PLATFORM}")
endif()

if(NOT EXISTS "${_work}/stage/lib/libopenblas.a"
   AND NOT EXISTS "${_work}/stage/lib/spectrum-viewer-openblas.lib")
    message(FATAL_ERROR "OpenBLAS staging did not produce a link library")
endif()

foreach(_license_pair
        "info/licenses/LICENSE|OpenBLAS-LICENSE.txt"
        "info/licenses/lapack-netlib/LICENSE|LAPACK-LICENSE.txt"
        "info/recipe/parent/recipe-scripts-license.txt|conda-forge-recipe-scripts-LICENSE.txt")
    string(REPLACE "|" ";" _license_parts "${_license_pair}")
    list(GET _license_parts 0 _license_source)
    list(GET _license_parts 1 _license_destination)
    if(NOT EXISTS "${_license_root}/${_license_source}")
        message(FATAL_ERROR "The OpenBLAS package omitted ${_license_source}")
    endif()
    configure_file(
        "${_license_root}/${_license_source}"
        "${_work}/stage/licenses/${_license_destination}" COPYONLY)
endforeach()

if(PLATFORM STREQUAL "windows")
    file(SHA256 "${_work}/stage/bin/spectrum-viewer-openblas.dll" _runtime_hash)
    set(_runtime_line
        "spectrum-viewer-openblas.dll SHA256=${_runtime_hash}\n")
else()
    set(_runtime_line "")
endif()
if(EXISTS "${_work}/stage/lib/libopenblas.a")
    file(SHA256 "${_work}/stage/lib/libopenblas.a" _library_hash)
    set(_library_name "libopenblas.a")
else()
    file(SHA256 "${_work}/stage/lib/spectrum-viewer-openblas.lib" _library_hash)
    set(_library_name "spectrum-viewer-openblas.lib")
endif()
file(WRITE "${_work}/stage/MANIFEST.txt"
    "Spectrum Viewer OpenBLAS stage v1\n"
    "OpenBLAS version: ${SV_OPENBLAS_VERSION}\n"
    "Source: https://github.com/OpenMathLib/OpenBLAS\n"
    "Binary source: conda-forge/openblas-feedstock\n"
    "Feedstock commit: ${SV_OPENBLAS_FEEDSTOCK_COMMIT}\n"
    "Platform: ${PLATFORM}\n"
    "Packages:\n${_packages}\n"
    "Files:\n${_library_name} SHA256=${_library_hash}\n${_runtime_line}")

file(RENAME "${_work}/stage" "${STAGE_DIR}")
file(REMOVE_RECURSE "${_work}")
message(STATUS "Prepared OpenBLAS in ${STAGE_DIR}")
