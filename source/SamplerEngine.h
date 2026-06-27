#pragma once

// dehli-musikk-sampler-engine — public facade.
//
// Milestones (see ../../PLAN.md):
//   M1 JSON manifest loader/model  ·  M2 voice engine + EmbeddedFlacSource
//   M3 FX + voice choke        ·  M6 auto-strum sequencer
//
// Include JUCE modules directly (no JuceHeader.h).
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "audio/VoiceEngine.h"
#include "audio/FxChain.h"

namespace dm
{

/** Top-level sampler engine. One instance per plugin; owns the active mode's
    voices, FX chain and (later) the note sequencer. Audio-thread-safe mode
    switching lands in M5.
*/
class SamplerEngine
{
public:
    SamplerEngine();
    ~SamplerEngine();

    /** Engine version string (handy for sanity-checking the build wiring). */
    static juce::String getVersion();

    /** Allocate for the given audio settings. Call before processBlock(). */
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);

    /** Configure playback from a manifest mode + a sample source (message thread).
        The source must outlive subsequent processBlock() calls. */
    void setMode (const Mode& mode, const SampleSource& source);

    /** Render one block (samples + amp ADSR for the active mode). */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi,
                       juce::AudioPlayHead* playHead);

    /** Release resources allocated in prepare(). */
    void releaseResources();

    int getActiveVoiceCount() const noexcept { return voiceEngine.getActiveVoiceCount(); }

    // --- FX control (thread-safe); addressed like manifest bindings ---
    void setEffectEnabled (int index, bool enabled)               { fxChain.setEffectEnabled (index, enabled); }
    void setEffectParam (int index, const juce::String& p, float v) { fxChain.setEffectParam (index, p, v); }
    int  getNumEffects() const noexcept                            { return fxChain.getNumEffects(); }

private:
    double currentSampleRate { 0.0 };
    int    currentBlockSize  { 0 };
    int    currentNumChannels { 0 };

    VoiceEngine voiceEngine;
    FxChain     fxChain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEngine)
};

} // namespace dm
