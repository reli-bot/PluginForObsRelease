# The OBS module: only the "atkAudio PluginHost" (VST3) filter.

add_library(${PROJECT_NAME} MODULE)

target_sources(
    ${PROJECT_NAME}
    PRIVATE
        plugin-main.cpp
        plugin_host.cpp
        core/atkaudio.cpp
        core/atkaudio/Logging.cpp
        core/atkaudio/ObsJucePluginFormatLifecycle.cpp
        core/atkaudio/SandboxedPluginScanner.cpp
        core/atkaudio/SharedPluginList.cpp
        core/atkaudio/ModuleInfrastructure/AudioServer/ChannelRoutingMatrix.cpp
        core/atkaudio/PluginHost/PluginHost.cpp
        core/atkaudio/PluginHost/Core/HostAudioProcessor.cpp
        core/atkaudio/PluginHost/UI/HostEditorWindow.cpp
        core/atkaudio/PluginHost/UI/NativeWindowOwner.cpp
)

configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/config.h" @ONLY)

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/core"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        OBS::libobs
        OBS::obs-frontend-api
        juce::juce_audio_processors
)

atk_apply_juce_target_settings(${PROJECT_NAME})

if(WIN32)
    set(PLUGIN_RC_FILE_DESCRIPTION "atkAudio PluginHost - VST3 plugin host filter for OBS ${OBS_TARGET_VERSION}")
    set(PLUGIN_RC_ORIGINAL_FILENAME "${PROJECT_NAME}.dll")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/windows/resources/resource.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROJECT_NAME}.rc"
        @ONLY
    )
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROJECT_NAME}.rc")
endif()

# OBS expects "<name>.dll" / "<name>.so" without a "lib" prefix.
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${PROJECT_NAME}")

# The scanner is a separate executable: plugins are scanned in their own process.
add_subdirectory(scanner)

# Ready-to-copy folder layout for C:\ProgramData\obs-studio\plugins\ (OBS 30+ user plugin location).
install(
    TARGETS
        ${PROJECT_NAME}
        ${PROJECT_NAME}_scanner
    RUNTIME DESTINATION "${PROJECT_NAME}/bin/64bit"
    LIBRARY DESTINATION "${PROJECT_NAME}/bin/64bit"
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/data/" DESTINATION "${PROJECT_NAME}/data")
if(MSVC)
    install(
        FILES
            $<TARGET_PDB_FILE:${PROJECT_NAME}>
            $<TARGET_PDB_FILE:${PROJECT_NAME}_scanner>
        DESTINATION "pdb"
        OPTIONAL
    )
endif()
