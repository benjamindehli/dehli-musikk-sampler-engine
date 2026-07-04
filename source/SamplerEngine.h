#pragma once

// dehli-musikk-sampler-engine — public facade.
//
// Milestones (see ../../PLAN.md):
//   M1 JSON manifest loader/model  ·  M2 voice engine + EmbeddedFlacSource
//   M3 FX (lowpass + convolution)  ·  M5 mode switching  ·  M6 auto-strum
//
// M5: the engine holds the whole PresetLibrary and renders one active mode. Each
// mode is a self-contained render unit (voices + FX) built on the message thread;
// switching is a lock-free pointer swap adopted by the audio thread, with the
// retired unit freed back on the message thread (no audio-thread allocation/locks).

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "audio/VoiceEngine.h"
#include "audio/FxChain.h"
#include "audio/NoteSequencer.h"

#include <atomic>

namespace dm
{

class SamplerEngine
{
public:
    SamplerEngine();
    ~SamplerEngine();

    /** Engine version string (handy for sanity-checking the build wiring). */
    static juce::String getVersion();

    /** Allocate for the given audio settings (message thread, audio stopped). */
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);

    /** Provide the manifest library + sample source (both kept alive by the
        caller). Activates mode 0. Message thread, audio stopped. */
    void setLibrary (const PresetLibrary& library, SampleSource& source);

    int  getNumModes() const noexcept;
    juce::StringArray getModeNames() const;
    int  getActiveModeIndex() const noexcept { return activeModeIndex; }

    /** Switch the active mode. Safe to call while audio is running — builds the
        new mode off-thread and swaps it in lock-free. Message thread. */
    void setActiveMode (int index);

    /** Render one block (samples + amp ADSR + FX for the active mode). */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi,
                       juce::AudioPlayHead* playHead);

    void releaseResources();

    int getActiveVoiceCount() const noexcept;   // approximate (audio-thread state)

    // Temporary dev FX controls — mode-aware: applied each block to the active
    // mode's first lowpass/convolution. Replaced by M4's data-driven UI + APVTS.
    void setLowpassEnabled (bool enabled);
    void setEffectEnabled (int index, bool enabled);   // enable/bypass any effect slot (lowpass, convolution, ...)
    void setLowpassFrequency (float hz);
    void setReverbMix (float amount);
    void setReverbWetGainDb (float db);
    void setGain (float db);                 // gain effect (LEVEL, dB) — first gain slot
    void setWaveShaperDrive (float drive);   // wave_shaper FX_DRIVE — first wave_shaper slot
    void setWaveShaperOutput (float level);  // wave_shaper FX_OUTPUT_LEVEL — first wave_shaper slot

    /** Set a parameter on a specific instrument effect by its index (FX_MIX,
        FX_DRIVE, LEVEL, FX_OUTPUT_LEVEL). Lets several same-type effects (e.g. Echo
        + Room convolutions) be addressed independently. */
    void setEffectParam (int effectIndex, const juce::String& parameter, float value);

    /** Reload a convolution effect's IR (cabinet selector). MESSAGE THREAD only. */
    void setEffectIr (int effectIndex, const juce::String& irId);

    /** Per-group output gain in dB (group-level `gain` effect). */
    void setGroupGain (int groupIndex, float db);

    /** Set modulator `position`'s depth 0..1 (MOD_AMOUNT) / rate in Hz (FREQUENCY).
        A mode can have several modulators; `position` selects which (from the binding). */
    void setLfoDepth (int position, float depth);
    void setLfoRate  (int position, float hz);

    /** Override sequence playback rate (steps/sec) for the active mode; <= 0 uses
        each trigger's own rate. (The future StrumSpeed control.) */
    void setSequencerRate (double stepsPerSecond);

    /** Runtime SEQ_INDEX offset for the active mode (e.g. the chord-ordering menu). */
    void setSequencerIndexOffset (int offset);

    // Amp envelope + per-group volume overrides for the active mode (ENV_*, AMP_VOLUME).
    void setAmpAttack (float seconds);
    void setAmpDecay (float seconds);
    void setAmpSustain (float level);
    void setAmpRelease (float seconds);
    void setAmpVelTrack (float amount);   // global AMP_VEL_TRACK (velocity → volume)
    void setMasterVolume (float volume);                     // instrument-level AMP_VOLUME
    void setGroupVolume (int groupIndex, float volume);
    void setGroupTagVolume (int groupIndex, float volume);   // TAG_VOLUME mixer knobs
    void setGroupEnabled (int groupIndex, bool enabled);
    void setGroupTuning (int groupIndex, float semitones);   // GROUP_TUNING
    void setGroupEffectParam (int groupIndex, int effectIndex, const juce::String& parameter, float value);
    void setGroupAmp (int groupIndex, const juce::String& parameter, float value);   // per-group ENV_*

    /** Pitch-wheel bend range (semitones) for the active mode; applied per block. */
    void setPitchBendRange (float semitones) { pitchBendRange.store (semitones); }

    /** User master output gain (linear), applied AFTER everything else — independent
        of the preset's AMP_VOLUME master. Set per block from the plugin's Master fader. */
    void setMasterOutputGain (float gainLinear) noexcept { uiMasterGain = gainLinear; }

private:
    struct ModeRender
    {
        NoteSequencer sequencer;
        VoiceEngine   voices;
        FxChain       fx;

        // Sample/IR asset ids this mode acquired from the source (decoded PCM). The
        // source frees an id's PCM once no live ModeRender holds it — so RAM tracks
        // the active mode(s), not the whole library. Released on destruction (message
        // thread, after this mode is retired), keeping the audio thread's reads valid.
        SampleSource* src = nullptr;
        juce::StringArray held;
        ~ModeRender() { if (src != nullptr) for (const auto& id : held) src->release (id); }
    };

    ModeRender* buildMode (int index) const;   // message thread
    void setCurrentDirect (ModeRender* mr);     // message thread, audio stopped
    void applyFxOverrides (ModeRender& mr);     // audio thread
    void resetOverrides();                      // clear per-mode UI overrides

    const PresetLibrary* library { nullptr };
    SampleSource*        source  { nullptr };   // non-const: acquire/release decode/free PCM

    double sampleRate { 0.0 };
    int    maxBlockSize { 0 };
    int    numChannels { 2 };
    int    activeModeIndex { 0 };

    ModeRender* current { nullptr };                 // audio-thread owned
    std::atomic<ModeRender*> pending { nullptr };    // message → audio
    std::atomic<ModeRender*> retired { nullptr };    // audio → message (deferred free)

    // FX UI overrides: applied only once "touched", so untouched controls keep the
    // manifest defaults (and survive mode switches).
    struct FxOverride { std::atomic<bool> touched { false }; std::atomic<float> value { 0.0f }; };
    FxOverride ovLowpassEnabled, ovLowpassFreq, ovReverbMix, ovReverbGain;
    FxOverride ovGain, ovWaveDrive, ovWaveOutput, ovMasterVol;
    static constexpr int kMaxMods = 16;          // per-modulator MOD_AMOUNT / FREQUENCY overrides
    FxOverride ovLfoDepth[kMaxMods];
    FxOverride ovLfoRate[kMaxMods];
    FxOverride ovAmpAttack, ovAmpDecay, ovAmpSustain, ovAmpRelease, ovAmpVelTrack;
    static constexpr int kMaxGroupVol = 64;   // per-group override slots (drum libs have many groups)
    FxOverride ovGroupVol[kMaxGroupVol];
    FxOverride ovGroupTagVol[kMaxGroupVol];   // TAG_VOLUME mixer knobs
    FxOverride ovGroupGain[kMaxGroupVol];     // group-level gain effect (dB)
    FxOverride ovGroupEnabled[kMaxGroupVol];
    FxOverride ovGroupTuning[kMaxGroupVol];
    static constexpr int kMaxEffects = 8;
    FxOverride ovEffectEnabled[kMaxEffects];
    // Per-effect-index params, so several same-type instrument effects are independent
    // (e.g. a separate lowpass + highpass, each with its own runtime cutoff).
    FxOverride ovEffectMix[kMaxEffects];
    FxOverride ovEffectDrive[kMaxEffects];
    FxOverride ovEffectLevel[kMaxEffects];
    FxOverride ovEffectOutput[kMaxEffects];
    FxOverride ovEffectFreq[kMaxEffects];   // FX_FILTER_FREQUENCY per effect index
    FxOverride ovEffectReso[kMaxEffects];   // FX_FILTER_RESONANCE
    FxOverride ovEffectModRate[kMaxEffects];   // FX_MOD_RATE  (chorus/phaser rate)
    FxOverride ovEffectModDepth[kMaxEffects];  // FX_MOD_DEPTH (chorus/phaser depth)
    FxOverride ovEffectFeedback[kMaxEffects];  // FX_FEEDBACK  (phaser feedback / chorus)
    // Selected convolution IR per effect (cabinet menu); message-thread only.
    // Re-applied when a mode is (re)built so the selection survives mode switches.
    juce::String desiredIr[kMaxEffects];

    std::atomic<bool>  ovSequencerRateTouched { false };
    std::atomic<double> ovSequencerRate { 0.0 };
    std::atomic<bool>  ovSequencerIndexTouched { false };
    std::atomic<int>    ovSequencerIndex { 0 };

    std::atomic<float> pitchBendRange { 2.0f };   // semitones; applied to current voices per block

    // Gain stages in processBlock (audio thread): the per-library trim (--gain) applied
    // BEFORE the FX (DecentSampler reduces level ahead of its effects, so the level-
    // dependent wave_shaper sees DS's moderate signal), and the master volume (instrument
    // AMP_VOLUME — an output control) applied AFTER the FX.
    float libraryGain { 1.0f };
    float masterGain { 1.0f };
    float uiMasterGain { 1.0f };   // user master output fader (post-everything)

    juce::MidiBuffer sequencedMidi;   // scratch: sequencer output → voices

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEngine)
};

} // namespace dm
