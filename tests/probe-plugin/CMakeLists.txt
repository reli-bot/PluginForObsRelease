# Two tiny VST3 test plugins used by the CI test that runs OBS 31.0.4 with this module.
# AtkProbeA multiplies audio by its "Gain" parameter (default 0.5), AtkProbeB (default 1.0) sits
# after it and measures what actually came out of the first PluginHost filter. Both append what
# they see to probe<N>.log in %ATK_PROBE_LOG_DIR%.

cmake_minimum_required(VERSION 3.28)

project(AtkProbePlugins VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(FetchContent)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(juce)

function(add_probe_plugin target plugin_code probe_id default_gain)
    juce_add_plugin(
        ${target}
        COMPANY_NAME "atkTest"
        PLUGIN_MANUFACTURER_CODE Atkt
        PLUGIN_CODE ${plugin_code}
        FORMATS VST3
        PRODUCT_NAME "${target}"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        COPY_PLUGIN_AFTER_BUILD FALSE
    )
    target_sources(${target} PRIVATE ProbePlugin.cpp)
    target_compile_definitions(
        ${target}
        PUBLIC
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0
            JUCE_VST3_CAN_REPLACE_VST2=0
            ATK_PROBE_ID=${probe_id}
            ATK_PROBE_DEFAULT_GAIN=${default_gain}f
    )
    target_link_libraries(
        ${target}
        PRIVATE
            juce::juce_audio_processors
        PUBLIC
            juce::juce_recommended_config_flags
    )
endfunction()

add_probe_plugin(AtkProbeA Prba 1 0.5)
add_probe_plugin(AtkProbeB Prbb 2 1.0)
