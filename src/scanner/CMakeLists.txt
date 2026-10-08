# Plugin scanner: a separate console executable that loads one plugin file, lists the plugins in
# it and prints them as XML. Started by the OBS module for every file during a user-requested scan.

add_executable(${PROJECT_NAME}_scanner PluginScanner.cpp)

target_link_libraries(${PROJECT_NAME}_scanner PRIVATE juce::juce_audio_processors)
atk_apply_juce_target_settings(${PROJECT_NAME}_scanner)

if(WIN32)
    # Console subsystem; the module starts it hidden and reads its stdout.
    set_target_properties(${PROJECT_NAME}_scanner PROPERTIES WIN32_EXECUTABLE FALSE)

    set(PLUGIN_RC_FILE_DESCRIPTION "atkAudio PluginHost VST3 plugin scanner")
    set(PLUGIN_RC_ORIGINAL_FILENAME "${PROJECT_NAME}_scanner.exe")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/windows/resources/resource.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROJECT_NAME}_scanner.rc"
        @ONLY
    )
    target_sources(${PROJECT_NAME}_scanner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${PROJECT_NAME}_scanner.rc")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME}_scanner PRIVATE pthread)
endif()

add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_scanner)
