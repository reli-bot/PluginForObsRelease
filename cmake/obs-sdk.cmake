# OBS Studio SDK for the plugin: headers from the obs-studio sources of OBS_TARGET_VERSION and, on
# Windows, import libraries generated from the export lists (.def) of the official
# OBS_TARGET_VERSION binaries (cmake/obs-sdk/<version>/*.def). Nothing from OBS is compiled, and the
# plugin can only link against functions that the real obs.dll / obs-frontend-api.dll export.
#
# Provides: OBS::libobs, OBS::obs-frontend-api

include_guard(GLOBAL)
include(FetchContent)

set(OBS_SOURCE_DIR "" CACHE PATH "Existing obs-studio source tree for OBS_TARGET_VERSION (optional)")

# (The CI workflow checks out the exact commit of the tag itself and passes OBS_SOURCE_DIR;
# 31.0.4 = 614fb6ae0902c695bf2a6f7670c09b76605e904a.)
if(NOT OBS_SOURCE_DIR)
    FetchContent_Declare(
        obs_studio_sources
        GIT_REPOSITORY https://github.com/obsproject/obs-studio.git
        GIT_TAG ${OBS_TARGET_VERSION}
        GIT_SHALLOW TRUE
        GIT_SUBMODULES ""
        # Headers only: never add obs-studio's own CMake project.
        SOURCE_SUBDIR "__headers_only__"
    )
    FetchContent_MakeAvailable(obs_studio_sources)
    set(OBS_SOURCE_DIR "${obs_studio_sources_SOURCE_DIR}")
endif()

if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
    message(FATAL_ERROR "OBS_SOURCE_DIR does not contain libobs/obs-module.h: ${OBS_SOURCE_DIR}")
endif()

file(STRINGS "${OBS_SOURCE_DIR}/libobs/obs-config.h" _obs_api_lines REGEX "^#define LIBOBS_API_(MAJOR|MINOR|PATCH)_VER ")
set(_obs_api_version "")
foreach(_line IN LISTS _obs_api_lines)
    string(REGEX REPLACE "^#define LIBOBS_API_[A-Z]+_VER ([0-9]+).*$" "\\1" _part "${_line}")
    list(APPEND _obs_api_version "${_part}")
endforeach()
list(JOIN _obs_api_version "." _obs_api_version)
if(NOT _obs_api_version STREQUAL OBS_TARGET_VERSION)
    message(FATAL_ERROR "libobs headers are version ${_obs_api_version}, expected ${OBS_TARGET_VERSION}")
endif()
message(STATUS "Using libobs ${_obs_api_version} headers from ${OBS_SOURCE_DIR}")

# OBS 31.0.x keeps the frontend API in UI/obs-frontend-api; 31.1+ moved it to frontend/api.
set(_obs_frontend_api_dir "${OBS_SOURCE_DIR}/UI/obs-frontend-api")
if(NOT EXISTS "${_obs_frontend_api_dir}/obs-frontend-api.h")
    set(_obs_frontend_api_dir "${OBS_SOURCE_DIR}/frontend/api")
endif()
if(NOT EXISTS "${_obs_frontend_api_dir}/obs-frontend-api.h")
    message(FATAL_ERROR "obs-frontend-api.h not found in ${OBS_SOURCE_DIR}")
endif()

add_library(obs_sdk_libobs INTERFACE)
target_include_directories(obs_sdk_libobs SYSTEM INTERFACE "${OBS_SOURCE_DIR}/libobs")

add_library(obs_sdk_frontend_api INTERFACE)
target_include_directories(obs_sdk_frontend_api SYSTEM INTERFACE "${_obs_frontend_api_dir}")
target_link_libraries(obs_sdk_frontend_api INTERFACE obs_sdk_libobs)

if(WIN32)
    target_include_directories(obs_sdk_libobs SYSTEM INTERFACE "${OBS_SOURCE_DIR}/deps/w32-pthreads")

    set(_obs_def_dir "${CMAKE_CURRENT_LIST_DIR}/obs-sdk/${OBS_TARGET_VERSION}")
    set(_obs_lib_dir "${CMAKE_BINARY_DIR}/obs-sdk")
    file(MAKE_DIRECTORY "${_obs_lib_dir}")

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_obs_machine X64)
    else()
        message(FATAL_ERROR "Only 64-bit Windows builds are supported")
    endif()

    if(CMAKE_AR)
        set(_obs_lib_tool "${CMAKE_AR}")
        set(_obs_lib_tool_args "")
    else()
        set(_obs_lib_tool "${CMAKE_LINKER}")
        set(_obs_lib_tool_args "/LIB")
    endif()

    foreach(_obs_lib IN ITEMS obs obs-frontend-api)
        set(_def "${_obs_def_dir}/${_obs_lib}.def")
        set(_implib "${_obs_lib_dir}/${_obs_lib}.lib")

        if(NOT EXISTS "${_def}")
            message(FATAL_ERROR "Missing export list ${_def}")
        endif()

        execute_process(
            COMMAND
                "${_obs_lib_tool}" ${_obs_lib_tool_args} /NOLOGO /MACHINE:${_obs_machine} "/DEF:${_def}"
                "/OUT:${_implib}"
            RESULT_VARIABLE _rc
            OUTPUT_VARIABLE _out
            ERROR_VARIABLE _err
        )
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_implib}")
            message(FATAL_ERROR "Creating ${_implib} failed (${_rc}):\n${_out}\n${_err}")
        endif()
    endforeach()

    target_link_libraries(obs_sdk_libobs INTERFACE "${_obs_lib_dir}/obs.lib")
    target_link_libraries(obs_sdk_frontend_api INTERFACE "${_obs_lib_dir}/obs-frontend-api.lib")
    message(STATUS "Generated OBS ${OBS_TARGET_VERSION} import libraries in ${_obs_lib_dir}")
endif()

add_library(OBS::libobs ALIAS obs_sdk_libobs)
add_library(OBS::obs-frontend-api ALIAS obs_sdk_frontend_api)
