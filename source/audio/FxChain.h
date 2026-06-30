#pragma once

// dehli-musikk-sampler-engine — fixed insert FX chain (M3).
//
// Builds an ordered chain from a mode's effects list and runs it over the mixed
// voice output. M3 implements `lowpass` (state-variable TPT filter) and
// `convolution` (reverb; IR resolved from the SampleSource). `gain` and other
// types pass through for now.
//
// Parameters are settable at runtime via atomics (message thread) and read by
// process() (audio thread), addressed by the same vocabulary as manifest bindings
// (FX_FILTER_FREQUENCY, FX_MIX, ENABLED) — which is what M4 will drive. Configuring
// the chain (setEffects) is message-thread only for now; M5 adds atomic swapping.

#include "SampleSource.h"
#include <model/Manifest.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <memory>
#include <vector>

namespace dm
{

class FxChain
{
public:
    FxChain() = default;

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void reset();

    /** Build the chain from a mode's effects. Convolution IRs are resolved from
        `source` (by the effect's `ir` asset id). Message thread only. */
    void setEffects (const juce::Array<Effect>& effects, const SampleSource& source);

    /** Apply the chain in place over the mixed voice output (audio thread). */
    void process (juce::AudioBuffer<float>& buffer);

    // --- runtime control (thread-safe) ---
    void setEffectEnabled (int index, bool enabled);
    void setEffectParam (int index, const juce::String& parameter, float value);
    int  getNumEffects() const noexcept { return (int) slots.size(); }

    /** Reload a convolution slot's impulse response at runtime (e.g. a cabinet
        selector). MESSAGE THREAD only — juce::dsp::Convolution loads the IR on its
        own background thread and swaps atomically, so it's safe alongside process(). */
    void setEffectIr (int index, const SampleSource& source, const juce::String& irId);

    // Semantic helpers that address the first effect of a kind (mode-agnostic),
    // used by the temporary dev FX controls.
    void setLowpassEnabled (bool enabled);
    void setLowpassFrequency (float hz);
    void setReverbMix (float amount);
    void setReverbWetGainDb (float db);
    void setGain (float db);                 // gain effect (level in dB)
    void setWaveShaperDrive (float drive);   // wave_shaper input drive
    void setWaveShaperOutput (float level);  // wave_shaper output level (clamped 0..1)

private:
    enum class Kind { passthrough, lowpass, convolution, gain, waveShaper };

    struct Slot
    {
        Kind kind { Kind::passthrough };
        std::atomic<bool>  enabled { true };
        std::atomic<float> frequency { 20000.0f };  // lowpass cutoff (Hz)
        std::atomic<float> resonance { 0.707f };     // lowpass Q
        std::atomic<float> mix { 0.0f };             // convolution dry/wet 0..1 (1 = fully wet)
        std::atomic<float> wetGainDb { 0.0f };       // convolution wet trim (dB, ±) to balance vs dry
        std::atomic<float> gainLinear { 1.0f };      // gain effect (linear)
        std::atomic<float> drive { 1.0f };           // wave_shaper input gain
        std::atomic<float> output { 1.0f };          // wave_shaper output level (0..1)
        bool normalize { true };                     // convolution: normalise the IR (off = as recorded)

        juce::dsp::StateVariableTPTFilter<float> filter;
        std::unique_ptr<juce::dsp::Convolution> convolution;
    };

    juce::dsp::ProcessSpec spec {};
    bool prepared { false };
    std::vector<std::unique_ptr<Slot>> slots;
    juce::AudioBuffer<float> dryBuffer;   // scratch for convolution dry/wet blend

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxChain)
};

} // namespace dm
