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
    void setLibrary (const PresetLibrary& library, const SampleSource& source);

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
    void setGroupVolume (int groupIndex, float volume);
    void setGroupEnabled (int groupIndex, bool enabled);
    void setGroupTuning (int groupIndex, float semitones);   // GROUP_TUNING

    /** Pitch-wheel bend range (semitones) for the active mode; applied per block. */
    void setPitchBendRange (float semitones) { pitchBendRange.store (semitones); }

private:
    struct ModeRender
    {
        NoteSequencer sequencer;
        VoiceEngine   voices;
        FxChain       fx;
    };

    ModeRender* buildMode (int index) const;   // message thread
    void setCurrentDirect (ModeRender* mr);     // message thread, audio stopped
    void applyFxOverrides (ModeRender& mr);     // audio thread
    void resetOverrides();                      // clear per-mode UI overrides

    const PresetLibrary* library { nullptr };
    const SampleSource*  source  { nullptr };

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
    FxOverride ovAmpAttack, ovAmpDecay, ovAmpSustain, ovAmpRelease;
    static constexpr int kMaxGroupVol = 64;   // per-group override slots (drum libs have many groups)
    FxOverride ovGroupVol[kMaxGroupVol];
    FxOverride ovGroupEnabled[kMaxGroupVol];
    FxOverride ovGroupTuning[kMaxGroupVol];
    static constexpr int kMaxEffects = 8;
    FxOverride ovEffectEnabled[kMaxEffects];

    std::atomic<bool>  ovSequencerRateTouched { false };
    std::atomic<double> ovSequencerRate { 0.0 };
    std::atomic<bool>  ovSequencerIndexTouched { false };
    std::atomic<int>    ovSequencerIndex { 0 };

    std::atomic<float> pitchBendRange { 2.0f };   // semitones; applied to current voices per block

    juce::MidiBuffer sequencedMidi;   // scratch: sequencer output → voices

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEngine)
};

} // namespace dm
