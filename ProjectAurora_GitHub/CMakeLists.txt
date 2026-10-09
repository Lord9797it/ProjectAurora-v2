cmake_minimum_required(VERSION 3.22)
project(ProjectAurora VERSION 0.1.0 LANGUAGES CXX)

if(NOT DEFINED JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE_DIR must point to a JUCE checkout containing CMakeLists.txt")
endif()
add_subdirectory("${JUCE_DIR}" JUCE)

juce_add_plugin(ProjectAurora
    COMPANY_NAME "Aurora Audio"
    BUNDLE_ID "audio.aurora.projectaurora"
    PLUGIN_MANUFACTURER_CODE AuRa
    PLUGIN_CODE AuP1
    FORMATS VST3 Standalone
    PRODUCT_NAME "Project Aurora"
    DESCRIPTION "Experimental real-time vocal pitch correction"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(ProjectAurora)
target_sources(ProjectAurora PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
)
target_compile_features(ProjectAurora PRIVATE cxx_std_17)
target_compile_definitions(ProjectAurora PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)
target_link_libraries(ProjectAurora PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags
)
