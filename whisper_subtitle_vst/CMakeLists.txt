cmake_minimum_required(VERSION 3.22)

# On Apple Silicon, build arm64 even if CMake guesses x86_64 (e.g. a Rosetta shell),
# otherwise native DAWs such as Logic won't load the plugin.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    execute_process(COMMAND sysctl -n hw.optional.arm64
                    OUTPUT_VARIABLE HOST_HAS_ARM64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(HOST_HAS_ARM64 STREQUAL "1")
        set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "Target architecture" FORCE)
    endif()
endif()

project(WhisperSubtitles VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
endif()

include(FetchContent)

# ---- JUCE -------------------------------------------------------------------
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(juce)

# ---- whisper.cpp ------------------------------------------------------------
set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
# Keep the binary portable across CPUs; turn on for a faster local-only build.
option(WHISPER_NATIVE "Optimise whisper.cpp for this machine's CPU" OFF)
set(GGML_NATIVE ${WHISPER_NATIVE} CACHE BOOL "" FORCE)

FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG        v1.9.5
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(whisper)

# ---- Plugin -----------------------------------------------------------------
set(PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND PLUGIN_FORMATS AU)
endif()

juce_add_plugin(WhisperSubtitles
    COMPANY_NAME            "LeapMotion"
    PLUGIN_MANUFACTURER_CODE Lmtn
    PLUGIN_CODE             Wsub
    PRODUCT_NAME            "Whisper Subtitles"
    FORMATS                 ${PLUGIN_FORMATS}
    IS_SYNTH                FALSE
    NEEDS_MIDI_INPUT        FALSE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES         Fx Analyzer
    AU_MAIN_TYPE            kAudioUnitType_Effect)

target_sources(WhisperSubtitles PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/AudioCapture.cpp
    Source/Transcriber.cpp
    Source/Subtitles.cpp)

target_compile_definitions(WhisperSubtitles PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(WhisperSubtitles
    PRIVATE
        whisper
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---- Headless end-to-end test ----------------------------------------------
option(WHISPER_SUBTITLES_TESTS "Build the headless end-to-end test" OFF)

if(WHISPER_SUBTITLES_TESTS)
    juce_add_console_app(WhisperSubtitlesTest PRODUCT_NAME "WhisperSubtitlesTest")
    target_sources(WhisperSubtitlesTest PRIVATE tests/EndToEndTest.cpp)
    target_include_directories(WhisperSubtitlesTest PRIVATE Source)
    target_compile_definitions(WhisperSubtitlesTest PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        WHISPER_SAMPLES_DIR="${whisper_SOURCE_DIR}/samples")
    target_link_libraries(WhisperSubtitlesTest PRIVATE
        WhisperSubtitles
        juce::juce_audio_formats
        juce::juce_recommended_config_flags)
endif()
