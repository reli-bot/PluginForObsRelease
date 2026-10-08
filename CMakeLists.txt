cmake_minimum_required(VERSION 3.28)

# atkAudio Plugin for OBS - VST3-only build.
#
# Builds just the "atkAudio PluginHost" audio filter (one VST3 plugin per filter) plus the
# out-of-process plugin scanner. It links only libobs and obs-frontend-api (plain C APIs) of
# OBS Studio ${OBS_TARGET_VERSION}; there is no Qt dependency, so it does not care which Qt
# version the OBS installation ships.

project(atkaudio-pluginforobs VERSION 0.34.8 LANGUAGES C CXX)

set(PLUGIN_DISPLAY_NAME "atkAudio Plugin (VST3-only)")
set(PLUGIN_AUTHOR "atkAudio")
set(PLUGIN_WEBSITE "https://www.atkaudio.com")
string(TIMESTAMP PLUGIN_YEAR "%Y")

set(OBS_TARGET_VERSION "31.0.4" CACHE STRING "OBS Studio version whose API is used")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
# No C++20 modules are used; skip the per-file module dependency scan.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
    # Same runtime as OBS itself (DLL CRT), deterministic output.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    add_compile_options(
        /W3
        /utf-8
        /permissive-
        /Brepro
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    )
    add_compile_definitions(
        UNICODE
        _UNICODE
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
    )
    add_link_options(
        /DEBUG
        /Brepro
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    )
elseif(NOT WIN32)
    message(
        WARNING
        "This VST3-only build targets Windows. Non-Windows builds are only useful as a compile check: "
        "without Qt nothing drives the JUCE message loop there."
    )
endif()

include(cmake/obs-sdk.cmake)
include(cmake/juce.cmake)

add_subdirectory(src)
