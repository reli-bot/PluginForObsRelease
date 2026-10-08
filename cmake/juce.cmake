# JUCE (modules only), same version and local patch as the upstream atkAudio build.

include_guard(GLOBAL)
include(FetchContent)

set(JUCE_MODULES_ONLY ON CACHE BOOL "Only build JUCE modules" FORCE)

FetchContent_Declare(
    juce
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE
    PATCH_COMMAND
        ${CMAKE_COMMAND} -P "${CMAKE_SOURCE_DIR}/cmake/patches/apply-juce-patches.cmake"
)
FetchContent_MakeAvailable(juce)

# Hosting configuration shared by the OBS module and the scanner: VST3 is the only plugin format.
set(ATK_JUCE_HOST_DEFINITIONS
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_ARA=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    # "Closed box": all networking is compiled out of JUCE (cmake/patches/juce-no-network.patch),
    # so neither the module nor the scanner contains or links any network code.
    JUCE_ATK_NO_NETWORK=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_WIN_PER_MONITOR_DPI_AWARE=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    # Assertions only in local debug builds (as upstream: release/CI builds disable them).
    $<$<NOT:$<CONFIG:Debug>>:JUCE_DISABLE_ASSERTIONS=1>
)

function(atk_apply_juce_target_settings target_name)
    target_compile_definitions(${target_name} PRIVATE ${ATK_JUCE_HOST_DEFINITIONS})
    target_link_libraries(${target_name} PRIVATE juce::juce_recommended_config_flags)

    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE
                /wd4244
                /wd4267
                /wd4390
                /wd5105
        )
    else()
        target_compile_options(
            ${target_name}
            PRIVATE
                -Wno-unused-parameter
                -Wno-sign-compare
                -Wno-unused-variable
        )
    endif()
endfunction()
