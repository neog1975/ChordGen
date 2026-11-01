cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(ChordGen VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHORDGEN_COPY_PLUGIN "Copy the plugin to the system plugin folder after building" ON)

add_subdirectory(JUCE)

set(CHORDGEN_SOURCES
    Source/Engine.cpp
    Source/Pitch.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

# One code base, two plugins (FL passes piano-roll notes only to instruments and audio only to effects):
#   ChordGen     instrument in the Channel Rack, hears the piano roll
#   ChordGen FX  effect on a mixer insert, hears an audio track or a microphone
function(chordgen_plugin target product code is_fx)
    if(is_fx)
        set(synth FALSE)
        set(categories Fx Analyzer)
    else()
        set(synth TRUE)
        set(categories Instrument Generator)
    endif()

    juce_add_plugin(${target}
        COMPANY_NAME "Apock"
        BUNDLE_ID com.apock.${target}
        PLUGIN_MANUFACTURER_CODE Apck
        PLUGIN_CODE ${code}
        PRODUCT_NAME "${product}"
        FORMATS VST3 Standalone
        IS_SYNTH ${synth}
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT TRUE
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "ChordGen listens to your melody to suggest chords."
        VST3_CATEGORIES ${categories}
        COPY_PLUGIN_AFTER_BUILD ${CHORDGEN_COPY_PLUGIN})

    target_sources(${target} PRIVATE ${CHORDGEN_SOURCES})
    target_compile_definitions(${target} PUBLIC
        CHORDGEN_FX=$<BOOL:${is_fx}>
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${target}
        PRIVATE
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endfunction()

chordgen_plugin(ChordGen "ChordGen" Chgn FALSE)
chordgen_plugin(ChordGenFX "ChordGen FX" Chfx TRUE)

option(CHORDGEN_TESTS "Build the capture/engine test program" OFF)
if(CHORDGEN_TESTS)
    juce_add_console_app(ChordGenTests PRODUCT_NAME "ChordGenTests")
    target_sources(ChordGenTests PRIVATE Tests/CaptureTest.cpp ${CHORDGEN_SOURCES})
    target_compile_definitions(ChordGenTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JucePlugin_Name="ChordGen" JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ChordGenTests PRIVATE juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)

    juce_add_console_app(PitchTests PRODUCT_NAME "PitchTests")
    target_sources(PitchTests PRIVATE Tests/PitchTest.cpp Source/Pitch.cpp)
    target_link_libraries(PitchTests PRIVATE juce::juce_core juce::juce_recommended_config_flags)
endif()
