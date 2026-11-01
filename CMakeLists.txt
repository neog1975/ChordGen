cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(ChordGen VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHORDGEN_COPY_PLUGIN "Copy the plugin to the system plugin folder after building" ON)

add_subdirectory(JUCE)

juce_add_plugin(ChordGen
    COMPANY_NAME "Apock"
    BUNDLE_ID com.apock.chordgen
    PLUGIN_MANUFACTURER_CODE Apck
    PLUGIN_CODE Chgn
    PRODUCT_NAME "ChordGen"
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    VST3_CATEGORIES Instrument Generator
    COPY_PLUGIN_AFTER_BUILD ${CHORDGEN_COPY_PLUGIN})

target_sources(ChordGen PRIVATE
    Source/Engine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(ChordGen PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(ChordGen
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

option(CHORDGEN_TESTS "Build the capture/engine test program" OFF)
if(CHORDGEN_TESTS)
    juce_add_console_app(ChordGenTests PRODUCT_NAME "ChordGenTests")
    target_sources(ChordGenTests PRIVATE
        Tests/CaptureTest.cpp
        Source/Engine.cpp
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp)
    target_compile_definitions(ChordGenTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="ChordGen" JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ChordGenTests PRIVATE juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
endif()
