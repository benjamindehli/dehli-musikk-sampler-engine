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
#include "params/CompiledMode.h"
#include "ui/ManifestEditor.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

        // Optional memory-mapped sample pack (samples.pak; its index is "<path>.json").
        // When set + present, samples are read from it instead of embedded BinaryData —
        // used by multi-GB libraries so the binary stays small. Images/manifest/IR still
        // come from findResource. Empty File = fully embedded (the default for small libs).
        juce::File packFile;
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
    float readOutputPeak() override { return outputPeakL.exchange (0.0f, std::memory_order_relaxed); }
    void readOutputPeaks (float& outL, float& outR) override
    {
        outL = outputPeakL.exchange (0.0f, std::memory_order_relaxed);
        outR = outputPeakR.exchange (0.0f, std::memory_order_relaxed);
    }
    juce::String getPluginVersion() const override { return assets.version; }
    juce::String getPluginName() const override { return getName(); }
    /** Embedding hosts (DMSE Studio) call this with false so the editor leaves
        the surrounding app window alone. Default true = plugin behaviour. */
    void setEditorManagesWindow (bool manages) { editorManagesWindow = manages; }
    bool manageTopLevelWindow() const override { return editorManagesWindow; }

    bool isStandaloneBuild() const override
    {
        // Belt and braces: the custom standalone app should stamp wrapperType, but
        // if either signal says standalone, there is no host transport to follow.
        return wrapperType == wrapperType_Standalone
            || juce::JUCEApplicationBase::isStandaloneApp();
    }
    bool needsModeChoice() const override { return modeChoicePending; }
    void confirmModeChoice (int index) override
    {
        modeChoicePending = false;
        if (auto* p = apvts != nullptr ? apvts->getParameter (params::id::mode) : nullptr)
            p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
        triggerAsyncUpdate();   // build even when the param value didn't change (mode 0)
    }
    bool hasAirSupply() const override { return engine.hasAirSupply(); }
    bool hasSequencer() const override
    {
        for (const auto& m : library.modes)
            if (! m.sequenceTriggers.isEmpty())
                return true;
        return false;
    }
    bool  isLoading() const override    { return engine.isLoading(); }
    float loadProgress() const override { return engine.loadProgress(); }
    void  pollEngine() override   // message-thread housekeeping (editor timer)
    {
        engine.drainRetired();
        // Finalize a pending MIDI learn: the audio thread only captured the CC
        // number; the mapping table + state tree are message-thread territory.
        if (const int cc = learnedCcPending.exchange (-1); cc >= 0 && learnParamId.isNotEmpty())
        {
            setMidiMapping (cc, learnParamId);
            learnParamId.clear();
        }
    }

    // ── MIDI learn (right-click a controller in the editor) ─────────────────
    void startMidiLearn (const juce::String& paramId) override
    {
        learnParamId = paramId;
        learnArmed.store (true);
    }
    void cancelMidiLearn() override
    {
        learnArmed.store (false);
        learnedCcPending.store (-1);
        learnParamId.clear();
    }
    bool isMidiLearnActive() const override { return learnArmed.load(); }
    int  getMidiMappingCc (const juce::String& paramId) const override
    {
        if (apvts == nullptr) return -1;
        const auto maps = apvts->state.getChildWithName (kMidiMapTag);
        for (int i = 0; i < maps.getNumChildren(); ++i)
            if (maps.getChild (i)["param"].toString() == paramId)
                return (int) maps.getChild (i)["cc"];
        return -1;
    }
    void removeMidiMapping (const juce::String& paramId) override
    {
        if (apvts == nullptr) return;
        auto maps = apvts->state.getChildWithName (kMidiMapTag);
        for (int i = maps.getNumChildren(); --i >= 0;)
            if (maps.getChild (i)["param"].toString() == paramId)
                maps.removeChild (i, nullptr);
        rebuildCcTargets();
    }
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
    void setMidiMapping (int cc, const juce::String& paramId);   // message thread
    void rebuildCcTargets();                                     // message thread
    void parameterChanged (const juce::String& paramID, float newValue) override;
    void handleAsyncUpdate() override;
    void applyMenuIrFor (int modeIndex);   // cabinet-menu FX_IR_FILE → engine (message thread)

    Assets assets;

    juce::MidiKeyboardState keyboardState;
    std::unique_ptr<EmbeddedFlacSource> sampleSource;
    PresetLibrary library;
    bool loaded { false };
    std::unordered_map<std::string, juce::Image> imageCache;   // decoded UI images by id (message thread)

    SamplerEngine engine;
    std::unique_ptr<juce::AudioProcessorValueTreeState> apvts;

    // Per-mode compiled binding plans (params/CompiledMode.h), built once in the
    // constructor (message thread; modes + APVTS layout never change afterwards).
    // processBlock indexes by the active mode — allocation-free apply.
    std::vector<std::unique_ptr<params::CompiledMode>> compiledModes;

    // Always-present global params, resolved once (string map lookups per block add up).
    std::atomic<float>* prmPitchBendUp    { nullptr };
    std::atomic<float>* prmPitchBendDown  { nullptr };
    std::atomic<float>* prmMasterOutput   { nullptr };
    std::atomic<float>* prmPitchDrift     { nullptr };
    std::atomic<float>* prmVolumeDrift    { nullptr };
    std::atomic<float>* prmSkipMuted      { nullptr };
    std::atomic<float>* prmMaxPolyphony   { nullptr };
    std::atomic<float>* prmSeqTempoSync   { nullptr };
    std::atomic<float>* prmSeqSyncDaw     { nullptr };
    std::atomic<float>* prmSeqBpm         { nullptr };
    std::atomic<float>* prmSeqNoteValue   { nullptr };
    std::atomic<float>* prmMasterTune     { nullptr };
    std::atomic<float>* prmVelocityCurve  { nullptr };
    std::atomic<float>* prmAirSupply      { nullptr };   // null when the library has no AirSupply

    // ── MIDI learn state ────────────────────────────────────────────────────
    // ccTargets is the audio thread's mapping table (atomics; message thread
    // rebuilds it). Mappings persist as a "midiMappings" child of the APVTS state.
    static constexpr const char* kMidiMapTag = "midiMappings";
    std::atomic<juce::RangedAudioParameter*> ccTargets[128] {};
    std::atomic<bool> learnArmed { false };
    std::atomic<int>  learnedCcPending { -1 };
    juce::String learnParamId;   // message thread only

    // Multi-mode fresh instances wait for the editor's mode chooser before decoding
    // anything; a DAW session restore confirms the saved mode instead (no popup).
    bool modeChoicePending = false;   // message thread
    bool editorManagesWindow = true;  // false when embedded in a larger app (Studio)

    std::atomic<int> uiPitchWheel { -1 };
    std::atomic<int> uiModWheel   { -1 };
    // Per-button click sequence (radio groups resolve to most-recently-clicked). Read on
    // the audio thread by CompiledMode::apply; bumped on the message thread by noteButtonClicked.
    static constexpr int kMaxUiButtons = params::kMaxUiButtons;   // single shared cap
    std::atomic<std::uint32_t> buttonClickSeq[kMaxUiButtons] {};
    std::atomic<std::uint32_t> clickCounter { 0 };
    std::atomic<float> outputPeakL { 0.0f };   // per-channel max |sample| since the editor last read
    std::atomic<float> outputPeakR { 0.0f };
    juce::StringArray irButtonParams;         // button params that swap a convolution IR

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestPluginProcessor)
};

} // namespace dm
