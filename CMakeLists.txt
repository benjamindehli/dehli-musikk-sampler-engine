# dehli-musikk-sampler-engine — shared sampler engine (static library).
#
# Consumes JUCE modules directly (no JuceHeader.h). Plugin/CLI products link
# against the `dehli_musikk_sampler_engine` target and include the engine's
# public headers directly (e.g. <SamplerEngine.h>).
#
# Normally added by the dehli-musikk-sample-plugins workspace, which fetches JUCE
# once for every sub-project. Configured on its own, as the engine's CI does, it
# fetches the same JUCE itself, so its tests can be built from this repo alone:
#   cmake -B build && cmake --build build --target dmse_tests
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.22)
    # The version CITATION.cff states. JUCE wants one for the test program.
    project(DehliMusikkSamplerEngine VERSION 1.0.0 LANGUAGES C CXX)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()
    # As the workspace sets them. Keep the JUCE tag the same as its
    # JUCE_GIT_TAG, so the tests here build against what the plugins ship with.
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        9.0.6
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
    enable_testing()
endif()

add_library(dehli_musikk_sampler_engine STATIC
    source/SamplerEngine.cpp
    source/ManifestPluginProcessor.cpp
    source/model/ManifestLoader.cpp
    source/model/ManifestWriter.cpp
    source/audio/EmbeddedFlacSource.cpp
    source/audio/VoiceEngine.cpp
    source/audio/FxChain.cpp
    source/audio/NoteSequencer.cpp
    source/ui/ManifestUiComponent.cpp
    source/ui/ManifestEditor.cpp
    source/params/ManifestParameters.cpp
    source/UpdateCheck.cpp
)

target_include_directories(dehli_musikk_sampler_engine
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source
)

# Linux VST3s are shared objects — every object linked into them must be position-
# independent. (No effect on macOS, where all code is PIC anyway.)
set_target_properties(dehli_musikk_sampler_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

# JUCE modules the engine needs. PUBLIC so consumers inherit the compile defs.
target_link_libraries(dehli_musikk_sampler_engine
    PUBLIC
        juce::juce_audio_basics
        juce::juce_audio_formats     # WAV/FLAC readers
        juce::juce_audio_processors
        juce::juce_dsp               # filter + convolution
        juce::juce_gui_basics        # data-driven UI renderer
        juce::juce_audio_utils       # MidiKeyboardComponent (ManifestEditor / ColouredKeyboard)
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# FLAC support in juce_audio_formats. The web browser is off, which is required
# on Linux, where juce_gui_extra otherwise demands GTK/WebKit dev headers.
#
# Curl is off too, except in the plugins dmse_add_plugin makes, which mark
# themselves with DMSE_USE_CURL. Their update check reaches an https address,
# and JUCE's Linux networking needs curl for that. Read on the target that
# consumes the engine, so DMSE Studio, the converter and the tests stay
# without curl and need no libcurl headers on Linux.
target_compile_definitions(dehli_musikk_sampler_engine
    PUBLIC
        JUCE_USE_FLAC=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=$<BOOL:$<TARGET_PROPERTY:DMSE_USE_CURL>>
)

# ---------------------------------------------------------------------------
# Unit tests (M1+): native JSON manifest loader. Built as a small console app
# and registered with CTest. Requires the JUCE CMake API, which is present when
# the engine is built via the top-level orchestrator that fetches JUCE.
#   cmake --build build --target dmse_tests && ctest --test-dir build
# ---------------------------------------------------------------------------
option(DMSE_BUILD_TESTS "Build dehli-musikk-sampler-engine unit tests" ON)

if(DMSE_BUILD_TESTS AND COMMAND juce_add_console_app)
    juce_add_console_app(dmse_tests PRODUCT_NAME "dmse-tests")

    target_sources(dmse_tests PRIVATE
        tests/ManifestLoaderTests.cpp
        tests/ManifestWriterTests.cpp
        tests/VoiceEngineTests.cpp
        tests/FxChainTests.cpp
        tests/SamplerEngineTests.cpp
        tests/NoteSequencerTests.cpp
        tests/CompiledModeTests.cpp
        tests/ManifestLintTests.cpp
        tests/AudioRegressionTests.cpp
        tests/SampleSourceTests.cpp
        tests/MissingSamplesTests.cpp
        tests/AccessibilityTests.cpp
        tests/StateRestoreTests.cpp
        tests/UpdateCheckTests.cpp
        tests/EditorScaleTests.cpp
    )

    # The tests and the engine they link, built with the address and undefined
    # behaviour sanitizers, for the sanitizers job in CI. Never the plugins.
    option(DMSE_SANITIZE "Build the engine tests with address and UB sanitizers" OFF)
    if(DMSE_SANITIZE AND NOT MSVC)
        foreach(target dmse_tests dehli_musikk_sampler_engine)
            # Frame pointers and no tail calls, so a report names every frame.
            target_compile_options(${target} PRIVATE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-optimize-sibling-calls
                -g)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endforeach()
    endif()

    target_link_libraries(dmse_tests PRIVATE
        dehli_musikk_sampler_engine
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    # Absolute path to the fixtures so the test can find them regardless of cwd.
    target_compile_definitions(dmse_tests PRIVATE
        DMSE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        DMSE_TEST_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    enable_testing()
    add_test(NAME dmse_tests COMMAND dmse_tests)
endif()
