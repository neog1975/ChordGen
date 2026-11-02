cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
project(ChordGen VERSION 0.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHORDGEN_COPY_PLUGIN "Copy the plugin to the system plugin folder after building" ON)

add_subdirectory(JUCE)

# Basic Pitch (Spotify's polyphonic transcription model) as embedded by NeuralNote, for ChordGen FX's
# audio-file import: the CNN runs on RTNeural, the CQT front end is a native port (ThirdParty/BasicPitch).
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW) # downloaded files get the time of extraction
endif()
FetchContent_Declare(rtneural
    URL https://github.com/jatinchowdhury18/RTNeural/archive/2ca066e5e529ea3ed4120c368ee16526eeaf2cec.tar.gz
    URL_HASH SHA256=7adaba69ff8c0935418e273127bb45c3d4cb35a0023c4255f6449010058b8fa1
    SOURCE_SUBDIR RTNeural) # the library only: RTNeural's top-level CMakeLists downloads CPM for its tests
FetchContent_MakeAvailable(rtneural)
target_compile_definitions(RTNeural PUBLIC RTNEURAL_USE_EIGEN=1 RTNEURAL_DEFAULT_ALIGNMENT=16)
target_include_directories(RTNeural SYSTEM PUBLIC ${rtneural_SOURCE_DIR}/modules/Eigen ${rtneural_SOURCE_DIR}/modules/json)
set_target_properties(RTNeural PROPERTIES SYSTEM TRUE)
if(MSVC)
    target_compile_options(RTNeural PRIVATE /w)
else()
    target_compile_options(RTNeural PRIVATE -w)
endif()

juce_add_binary_data(BasicPitchModel
    NAMESPACE BasicPitchData
    HEADER_NAME BasicPitchData.h
    SOURCES
        ThirdParty/BasicPitch/ModelData/cnn_contour_model.json
        ThirdParty/BasicPitch/ModelData/cnn_note_model.json
        ThirdParty/BasicPitch/ModelData/cnn_onset_1_model.json
        ThirdParty/BasicPitch/ModelData/cnn_onset_2_model.json
        ThirdParty/BasicPitch/ModelData/features_cqt.bin)

set(BASICPITCH_SOURCES
    ThirdParty/BasicPitch/BasicPitch.cpp
    ThirdParty/BasicPitch/BasicPitchCNN.cpp
    ThirdParty/BasicPitch/Features.cpp
    ThirdParty/BasicPitch/Notes.cpp)
set_source_files_properties(${BASICPITCH_SOURCES} PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
set(AUDIOIMPORT_SOURCES Source/AudioImport.cpp ${BASICPITCH_SOURCES})

# Adds audio-file import (sources, model data, modules) to a target that already has the JUCE modules.
function(chordgen_audio_import target)
    target_sources(${target} PRIVATE ${AUDIOIMPORT_SOURCES})
    target_include_directories(${target} SYSTEM PRIVATE ThirdParty/BasicPitch)
    target_link_libraries(${target} PRIVATE RTNeural BasicPitchModel juce::juce_audio_formats juce::juce_dsp)
endfunction()

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
    if(is_fx)
        chordgen_audio_import(${target})
    endif()
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

    juce_add_console_app(AudioImportTests PRODUCT_NAME "AudioImportTests")
    target_sources(AudioImportTests PRIVATE Tests/AudioImportTest.cpp Source/Engine.cpp)
    target_link_libraries(AudioImportTests PRIVATE juce::juce_audio_basics juce::juce_recommended_config_flags)
    chordgen_audio_import(AudioImportTests)
endif()

if(CHORDGEN_TESTS)
    juce_add_console_app(MidiBatch PRODUCT_NAME "MidiBatch")
    target_sources(MidiBatch PRIVATE Tests/MidiBatch.cpp Source/Engine.cpp)
    target_link_libraries(MidiBatch PRIVATE juce::juce_audio_basics juce::juce_recommended_config_flags)
endif()

if(CHORDGEN_TESTS)
    juce_add_console_app(ChordEval PRODUCT_NAME "ChordEval")
    target_sources(ChordEval PRIVATE Tests/ChordEval.cpp Source/Engine.cpp)
    target_link_libraries(ChordEval PRIVATE juce::juce_audio_basics juce::juce_recommended_config_flags)
endif()
