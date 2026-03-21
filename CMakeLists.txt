cmake_minimum_required(VERSION 3.22)
project(W2AudioPlugs VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Fetch JUCE ──────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.7
    GIT_SHALLOW    TRUE
)

# ── Fetch CLAP-JUCE extensions ───────────────────────────────────────────────
FetchContent_Declare(
    clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)

# ── Fetch Bungee (time-stretch / pitch-shift) ────────────────────────────────
FetchContent_Declare(
    bungee
    GIT_REPOSITORY    https://github.com/bungee-audio-stretch/bungee.git
    GIT_TAG           main
    GIT_SHALLOW       FALSE
    GIT_SUBMODULES_RECURSE TRUE
)

FetchContent_MakeAvailable(JUCE clap-juce-extensions bungee)

# ── Plugin definition ────────────────────────────────────────────────────────
juce_add_plugin(W2Sampler
    COMPANY_NAME            "W2Audio"
    PLUGIN_MANUFACTURER_CODE W2Au
    PLUGIN_CODE             W2S1
    FORMATS                 Standalone VST3 AU
    PRODUCT_NAME            "W2 Sampler"
    IS_SYNTH                TRUE
    NEEDS_MIDI_INPUT        TRUE
    NEEDS_MIDI_OUTPUT       FALSE
    IS_MIDI_EFFECT          FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES         "Instrument" "Sampler"
    AU_MAIN_TYPE            kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# ── Sources ──────────────────────────────────────────────────────────────────
target_sources(W2Sampler
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/EuclideanSequencer.cpp
        src/SamplerVoice.cpp
        src/SampleLibrary.cpp
)

target_include_directories(W2Sampler PRIVATE src ${bungee_SOURCE_DIR})

# ── JUCE modules ─────────────────────────────────────────────────────────────
target_link_libraries(W2Sampler
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        bungee_library
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ── CLAP target (wraps the VST3 build) ───────────────────────────────────────
clap_juce_extensions_plugin(
    TARGET              W2Sampler
    CLAP_ID             "com.w2audio.w2sampler"
    CLAP_FEATURES       instrument sampler
)
