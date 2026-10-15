cmake_minimum_required(VERSION 3.22)

# macOS: universal binary (Apple Silicon + Intel), runs on macOS 11 Big Sur and later.
# These must be set before project(); they are ignored on other platforms.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

# Windows: link the C++ runtime statically so users don't need the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(Quell VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Point this at a JUCE checkout:  -DJUCE_DIR=path/to/JUCE
# If not given, JUCE 9.0.4 is downloaded automatically.
set(JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout")

if(JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

# AU is for Logic Pro / GarageBand and only exists on macOS
if(APPLE)
    set(QUELL_FORMATS VST3 AU Standalone)
else()
    set(QUELL_FORMATS VST3 Standalone)
endif()

juce_add_plugin(Quell
    COMPANY_NAME "Quell Audio"
    BUNDLE_ID com.quellaudio.quell
    PLUGIN_MANUFACTURER_CODE Qlau
    PLUGIN_CODE Qul1
    FORMATS ${QUELL_FORMATS}
    VERSION ${PROJECT_VERSION}
    HARDENED_RUNTIME_ENABLED TRUE
    PRODUCT_NAME "Quell"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Dynamics EQ
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(Quell PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Quell PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Quell
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline DSP test (no GUI): checks transparency and resonance reduction
juce_add_console_app(QuellEngineTest PRODUCT_NAME "QuellEngineTest")
target_sources(QuellEngineTest PRIVATE tests/EngineTest.cpp)
target_include_directories(QuellEngineTest PRIVATE Source)
target_compile_definitions(QuellEngineTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(QuellEngineTest PRIVATE juce::juce_dsp juce::juce_audio_basics
    PUBLIC juce::juce_recommended_config_flags)

# Dev tool: renders the editor (after feeding it test audio) to a PNG
option(QUELL_BUILD_SNAPSHOT "Build the UI snapshot tool" OFF)
if(QUELL_BUILD_SNAPSHOT)
    juce_add_console_app(QuellSnapshot PRODUCT_NAME "QuellSnapshot")
    target_sources(QuellSnapshot PRIVATE tests/Snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(QuellSnapshot PRIVATE Source)
    target_compile_definitions(QuellSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="Quell" JucePlugin_VersionString="${PROJECT_VERSION}" JUCE_STANDALONE_APPLICATION=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(QuellSnapshot PRIVATE juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags)
endif()
