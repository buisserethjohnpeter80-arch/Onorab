# Onorab VST3 plugin - CMake build.
#
# Pinned dependency: JUCE 8.0.8 (FetchContent, tag - never an unversioned
# branch). This project is built by GitHub Actions on a Windows runner, so
# the user does NOT need Visual Studio installed locally to get a .vst3.
# A developer who does have VS 2022 can still build locally with the same
# commands (see README.md in this folder).
#
# Output: a Windows 64-bit VST3 named exactly "Onorab".
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# The .vst3 bundle lands under build/Onorab_artefacts/Release/VST3/.

cmake_minimum_required(VERSION 3.22)

project(OnorabAudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# JUCE 8.0.8, pinned by tag.
FetchContent_Declare(
  JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.8
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(Onorab
    COMPANY_NAME "Potorik Track"
    BUNDLE_ID "com.potoriktrack.onorab"
    PLUGIN_MANUFACTURER_CODE Ptrk
    PLUGIN_CODE Onrb
    FORMATS VST3
    PRODUCT_NAME "Onorab"
    DESCRIPTION "Onorab cloud audio bridge for FL Studio"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES "Fx"
)

target_sources(Onorab
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/HubClient.cpp
        Source/WaveformView.cpp
)

# NOTE (2026-11-04): an earlier revision passed /utf-8 to MSVC here to
# stop UTF-8 punctuation in UI strings turning into mojibake. That flag
# was the only build-system change in the redesign push whose Windows
# run then failed deterministically with C1083 (missing generated
# JuceHeader.h), so it is REVERTED as a controlled experiment: if the
# next run goes green, the flag was the trigger; if it stays red, the
# cause is environmental (runner/JUCE toolchain) and the fix lies
# elsewhere. The mojibake fix does NOT depend on this flag: all
# redesign UI strings are plain ASCII, which renders correctly under
# any MSVC source codepage.
target_compile_definitions(Onorab
    PRIVATE
        # No embedded web browser in the plugin UI.
        JUCE_WEB_BROWSER=0
        # Use the OS-native HTTP stack on Windows (WinHTTP) instead of
        # requiring libcurl. Hub traffic is localhost-only anyway.
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(Onorab
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_audio_basics
        juce::juce_gui_basics
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
