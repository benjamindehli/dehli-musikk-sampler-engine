#pragma once

// dehli-musikk-sampler-engine — public facade.
//
// M0 skeleton: the engine compiles and can be embedded in a plugin, but does not
// yet produce sound. Real DSP arrives in later milestones (see ../../PLAN.md):
//   M1 JSON manifest loader/model  ·  M2 voice engine + EmbeddedFlacSource
//   M3 FX + voice choke        ·  M6 auto-strum sequencer
//
// Include JUCE modules directly (no JuceHeader.h).
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

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

    /** Render one block. M0: writes silence. */
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer& midi,
                       juce::AudioPlayHead* playHead);

    /** Release resources allocated in prepare(). */
    void releaseResources();

private:
    double currentSampleRate { 0.0 };
    int    currentBlockSize  { 0 };
    int    currentNumChannels { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplerEngine)
};

} // namespace dm
