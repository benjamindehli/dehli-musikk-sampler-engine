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

private:
    enum class Kind { passthrough, lowpass, convolution };

    struct Slot
    {
        Kind kind { Kind::passthrough };
        std::atomic<bool>  enabled { true };
        std::atomic<float> frequency { 20000.0f };  // lowpass cutoff (Hz)
        std::atomic<float> resonance { 0.707f };     // lowpass Q
        std::atomic<float> mix { 0.0f };             // convolution wet amount 0..1

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
