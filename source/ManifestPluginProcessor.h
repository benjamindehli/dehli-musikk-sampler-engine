#pragma once

// Reusable plugin processor for a manifest+FLAC library (shared across products).
// Loads the embedded manifest + samples, owns the APVTS (built from the manifest),
// renders via SamplerEngine, hosts the data-driven editor, and persists state.
//
// Each plugin supplies only its embedded assets (its own BinaryData) + a name via
// the Assets struct, so a product is little more than:
//     createPluginFilter() { return new dm::ManifestPluginProcessor (assets); }
// Plugin-specific JucePlugin_* macros (formats, codes) come from juce_add_plugin.

#include <juce_audio_processors/juce_audio_processors.h>
#include "SamplerEngine.h"
#include "audio/EmbeddedFlacSource.h"
#include "model/ManifestLoader.h"
#include "params/ManifestParameters.h"
#include "ui/ManifestEditor.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace dm
{

class ManifestPluginProcessor : public juce::AudioProcessor,
                                public  ManifestEditorHost,
                                private juce::AudioProcessorValueTreeState::Listener,
                                private juce::AsyncUpdater
{
public:
    /** Everything a product brings to the shared processor. */
    struct Assets
    {
        juce::String name;                  // product name (JucePlugin_Name)
        juce::String version;               // plugin version (JucePlugin_VersionString), for the UI
        const char*  manifestJson = nullptr;
        int          manifestJsonSize = 0;
        // Look up an embedded resource by its original file name (e.g. "Bass_1C.flac",
        // "Knob.png") — the plugin's BinaryData lookup. Returns nullptr if absent.
        std::function<const char* (const juce::String& filename, int& sizeOut)> findResource;
    };

    explicit ManifestPluginProcessor (Assets assets);
    ~ManifestPluginProcessor() override;

    // --- AudioProcessor ------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return assets.name; }

    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override            { return 1; }
    int getCurrentProgram() override         { return 0; }
    void setCurrentProgram (int) override    {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- ManifestEditorHost --------------------------------------------------
    juce::AudioProcessorValueTreeState& getApvts() override { return *apvts; }
    juce::MidiKeyboardState& getKeyboardState() override    { return keyboardState; }
    juce::StringArray getModeNames() const override         { return engine.getModeNames(); }
    int  getActiveModeIndex() const override                { return engine.getActiveModeIndex(); }
    void setPitchWheel (int value14) override { uiPitchWheel.store (value14); }
    void setModWheel   (int value7)  override { uiModWheel.store (value7);  }
    juce::Image loadImage (const juce::String& id) override;
    const Mode* getActiveMode() const override
    {
        if (! loaded) return nullptr;
        const int i = engine.getActiveModeIndex();
        if (i < 0 || i >= library.modes.size()) return nullptr;
        return &library.modes.getReference (i);
    }
    float readOutputPeak() override { return outputPeak.exchange (0.0f, std::memory_order_relaxed); }
    juce::String getPluginVersion() const override { return assets.version; }
    juce::String getPluginName() const override { return getName(); }
    void noteButtonClicked (int buttonIndex) override
    {
        if (buttonIndex >= 0 && buttonIndex < kMaxUiButtons)
            buttonClickSeq[buttonIndex].store (clickCounter.fetch_add (1, std::memory_order_relaxed) + 1,
                                               std::memory_order_relaxed);
    }

protected:
    SamplerEngine& getEngine() noexcept { return engine; }
    bool isLoaded() const noexcept      { return loaded; }

private:
    void loadEmbeddedLibrary();
    void parameterChanged (const juce::String& paramID, float newValue) override;
    void handleAsyncUpdate() override;
    void applyMenuIrFor (int modeIndex);   // cabinet-menu FX_IR_FILE → engine (message thread)

    Assets assets;

    juce::MidiKeyboardState keyboardState;
    std::unique_ptr<EmbeddedFlacSource> sampleSource;
    PresetLibrary library;
    bool loaded { false };

    SamplerEngine engine;
    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;

    std::atomic<int> uiPitchWheel { -1 };
    std::atomic<int> uiModWheel   { -1 };
    // Per-button click sequence (radio groups resolve to most-recently-clicked). Read on
    // the audio thread by applyToEngine; bumped on the message thread by noteButtonClicked.
    static constexpr int kMaxUiButtons = 64;
    std::atomic<std::uint32_t> buttonClickSeq[kMaxUiButtons] {};
    std::atomic<std::uint32_t> clickCounter { 0 };
    std::atomic<float> outputPeak { 0.0f };   // max |sample| since the editor last read it
    juce::StringArray irButtonParams;         // button params that swap a convolution IR

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestPluginProcessor)
};

} // namespace dm
