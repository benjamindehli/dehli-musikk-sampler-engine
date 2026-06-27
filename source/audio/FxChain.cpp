#include "FxChain.h"

namespace dm
{

void FxChain::prepare (double sampleRate, int maxBlockSize, int numChannels)
{
    spec.sampleRate       = sampleRate > 0.0 ? sampleRate : 44100.0;
    spec.maximumBlockSize = (juce::uint32) juce::jmax (1, maxBlockSize);
    spec.numChannels      = (juce::uint32) juce::jmax (1, numChannels);
    prepared = true;

    for (auto& slotPtr : slots)
    {
        auto& s = *slotPtr;
        if (s.kind == Kind::lowpass)
            s.filter.prepare (spec);
        if (s.convolution != nullptr)
            s.convolution->prepare (spec);
    }

    dryBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);
}

void FxChain::reset()
{
    for (auto& slotPtr : slots)
    {
        if (slotPtr->kind == Kind::lowpass)
            slotPtr->filter.reset();
        if (slotPtr->convolution != nullptr)
            slotPtr->convolution->reset();
    }
}

void FxChain::setEffects (const juce::Array<Effect>& effects, const SampleSource& source)
{
    slots.clear();

    for (const auto& e : effects)
    {
        auto slot = std::make_unique<Slot>();
        slot->enabled.store (e.enabled);

        if (e.type == "lowpass")
        {
            slot->kind = Kind::lowpass;
            slot->frequency.store ((float) (e.frequency ? *e.frequency : 20000.0));
            slot->resonance.store ((float) (e.resonance ? *e.resonance : 0.707));
            slot->filter.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
            if (prepared)
            {
                slot->filter.prepare (spec);
                slot->filter.setCutoffFrequency (slot->frequency.load());
                slot->filter.setResonance (slot->resonance.load());
            }
        }
        else if (e.type == "convolution")
        {
            slot->kind = Kind::convolution;
            slot->mix.store ((float) (e.wet ? *e.wet : (e.mix ? *e.mix : 0.0)));
            slot->convolution = std::make_unique<juce::dsp::Convolution>();
            if (prepared)
                slot->convolution->prepare (spec);

            if (const auto* ir = source.get (e.ir); ir != nullptr && ir->getNumFrames() > 0)
            {
                juce::AudioBuffer<float> irCopy;
                irCopy.makeCopyOf (ir->audio);
                slot->convolution->loadImpulseResponse (
                    std::move (irCopy), ir->sampleRate,
                    ir->getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                             : juce::dsp::Convolution::Stereo::no,
                    juce::dsp::Convolution::Trim::no,
                    juce::dsp::Convolution::Normalise::yes);
            }
        }
        // else: gain / unknown → passthrough for M3.

        slots.push_back (std::move (slot));
    }
}

void FxChain::process (juce::AudioBuffer<float>& buffer)
{
    if (! prepared)
        return;

    const int numCh = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    juce::dsp::AudioBlock<float> block (buffer);

    for (auto& slotPtr : slots)
    {
        auto& s = *slotPtr;
        if (! s.enabled.load())
            continue;

        if (s.kind == Kind::lowpass)
        {
            const auto nyquist = (float) (spec.sampleRate * 0.49);
            s.filter.setCutoffFrequency (juce::jlimit (20.0f, nyquist, s.frequency.load()));
            s.filter.setResonance (juce::jmax (0.01f, s.resonance.load()));
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            s.filter.process (ctx);
        }
        else if (s.kind == Kind::convolution && s.convolution != nullptr)
        {
            const float mix = juce::jlimit (0.0f, 1.0f, s.mix.load());
            if (mix <= 0.0f)
                continue; // fully dry — skip the convolution entirely

            dryBuffer.makeCopyOf (buffer, true);

            juce::dsp::ProcessContextReplacing<float> ctx (block);
            s.convolution->process (ctx); // buffer is now fully wet

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                const auto* dry = dryBuffer.getReadPointer (juce::jmin (ch, dryBuffer.getNumChannels() - 1));
                for (int i = 0; i < numSamples; ++i)
                    wet[i] = dry[i] * (1.0f - mix) + wet[i] * mix;
            }
        }
    }
}

void FxChain::setEffectEnabled (int index, bool enabled)
{
    if (index >= 0 && index < (int) slots.size())
        slots[(size_t) index]->enabled.store (enabled);
}

void FxChain::setEffectParam (int index, const juce::String& parameter, float value)
{
    if (index < 0 || index >= (int) slots.size())
        return;

    auto& s = *slots[(size_t) index];
    if (parameter == "FX_FILTER_FREQUENCY")
        s.frequency.store (value);
    else if (parameter == "FX_MIX")
        s.mix.store (value);
    else if (parameter == "ENABLED")
        s.enabled.store (value > 0.5f);
    // FX_DRIVE / FX_OUTPUT_LEVEL / resonance arrive with their effects later.
}

} // namespace dm
