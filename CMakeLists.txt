# dehli-musikk-sampler-engine — shared sampler engine (static library).
#
# Consumes JUCE modules directly (no JuceHeader.h). Plugin/CLI products link
# against the `dehli_musikk_sampler_engine` target and include the engine's
# public headers directly (e.g. <SamplerEngine.h>).

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

# FLAC support in juce_audio_formats. Web/curl are off (same values the plugin
# targets set) — REQUIRED on Linux, where juce_core/juce_gui_extra otherwise
# demand libcurl and GTK/WebKit dev headers at compile time.
target_compile_definitions(dehli_musikk_sampler_engine
    PUBLIC
        JUCE_USE_FLAC=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
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
    )

    target_link_libraries(dmse_tests PRIVATE
        dehli_musikk_sampler_engine
        juce::juce_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
    )

    # Absolute path to the fixtures so the test can find them regardless of cwd.
    target_compile_definitions(dmse_tests PRIVATE
        DMSE_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
    )

    enable_testing()
    add_test(NAME dmse_tests COMMAND dmse_tests)
endif()
