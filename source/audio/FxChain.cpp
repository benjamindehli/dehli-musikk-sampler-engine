#include "FxChain.h"
#include <cmath>

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
        if (s.kind == Kind::lowpass || s.kind == Kind::highpass)   // both use `filter`
            s.filter.prepare (spec);
        if (s.convolution != nullptr)
            s.convolution->prepare (spec);
        if (s.chorusDelay != nullptr)
        {
            s.chorusDelay->setMaximumDelayInSamples ((int) (spec.sampleRate * 0.05) + 4);   // 50 ms headroom
            s.chorusDelay->prepare (spec);
            s.chorusDelay->reset();
            s.chorusPhase = 0.0;
            s.chorusLastWet[0] = s.chorusLastWet[1] = 0.0f;
        }
        if (s.phaser != nullptr)
            s.phaser->prepare (spec);
    }

    dryBuffer.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);
}

void FxChain::reset()
{
    for (auto& slotPtr : slots)
    {
        if (slotPtr->kind == Kind::lowpass || slotPtr->kind == Kind::highpass)
            slotPtr->filter.reset();
        if (slotPtr->convolution != nullptr)
            slotPtr->convolution->reset();
        if (slotPtr->chorusDelay != nullptr)
        {
            slotPtr->chorusDelay->reset();
            slotPtr->chorusPhase = 0.0;
            slotPtr->chorusLastWet[0] = slotPtr->chorusLastWet[1] = 0.0f;
        }
        if (slotPtr->phaser != nullptr)
            slotPtr->phaser->reset();
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
        else if (e.type == "highpass")
        {
            slot->kind = Kind::highpass;
            slot->frequency.store ((float) (e.frequency ? *e.frequency : 20.0));
            slot->resonance.store ((float) (e.resonance ? *e.resonance : 0.707));
            slot->filter.setType (juce::dsp::StateVariableTPTFilterType::highpass);
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
            slot->wetGainDb.store ((float) (e.outputLevel ? *e.outputLevel : 0.0));
            slot->normalize = e.normalizeIr;
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
                    slot->normalize ? juce::dsp::Convolution::Normalise::yes
                                    : juce::dsp::Convolution::Normalise::no);
            }
        }
        else if (e.type == "gain")
        {
            slot->kind = Kind::gain;
            // DecentSampler gain `level` is in dB by default (no levelUnit handling yet).
            slot->gainLinear.store (juce::Decibels::decibelsToGain ((float) (e.gain ? *e.gain : 0.0)));
        }
        else if (e.type == "wave_shaper")
        {
            slot->kind = Kind::waveShaper;
            slot->drive.store  ((float) (e.drive       ? *e.drive       : 1.0));
            slot->output.store ((float) (e.outputLevel ? *e.outputLevel : 1.0));
        }
        else if (e.type == "chorus")
        {
            // Stereo widener / ensemble. mix is dynamic (a mono/stereo switch drives
            // FX_MIX); rate/depth/feedback are static from the effect (gentle defaults
            // if the preset omits them). Implemented as an anti-phase per-channel
            // modulated delay (see process) so it actually widens a mono source.
            slot->kind = Kind::chorus;
            slot->mix.store ((float) (e.mix ? *e.mix : 0.5));
            slot->modRate.store     ((float) (e.rate  ? *e.rate  : 1.0));
            slot->modDepth.store    ((float) (e.depth ? *e.depth : 0.25));
            slot->modFeedback.store (juce::jlimit (-0.95f, 0.95f, (float) (e.feedback ? *e.feedback : 0.0)));
            slot->chorusDelay = std::make_unique<
                juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear>>();
            if (prepared)
            {
                slot->chorusDelay->setMaximumDelayInSamples ((int) (spec.sampleRate * 0.05) + 4);
                slot->chorusDelay->prepare (spec);
                slot->chorusDelay->reset();
            }
        }
        else if (e.type == "phaser")
        {
            // Allpass sweep. mix is dynamic (On button drives FX_MIX); rate/depth/feedback
            // are the Rate/Color controls (FX_MOD_RATE / FX_MOD_DEPTH / FX_FEEDBACK).
            slot->kind = Kind::phaser;
            slot->mix.store         ((float) (e.mix ? *e.mix : 1.0));
            slot->modRate.store     ((float) (e.rate  ? *e.rate  : 0.5));
            slot->modDepth.store    ((float) (e.depth ? *e.depth : 0.5));
            slot->modFeedback.store (juce::jlimit (-0.95f, 0.95f, (float) (e.feedback ? *e.feedback : 0.0)));
            slot->phaser = std::make_unique<juce::dsp::Phaser<float>>();
            if (prepared)
                slot->phaser->prepare (spec);
        }
        // else: unknown → passthrough.

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

        if (s.kind == Kind::lowpass || s.kind == Kind::highpass)
        {
            // Only touch the filter when a value actually moved — setCutoffFrequency
            // recomputes tan() every call, and this runs every block forever.
            const auto nyquist = (float) (spec.sampleRate * 0.49);
            const float f = juce::jlimit (20.0f, nyquist, s.frequency.load());
            const float q = juce::jmax (0.01f, s.resonance.load());
            if (! juce::exactlyEqual (f, s.lastFreq)) { s.filter.setCutoffFrequency (f); s.lastFreq = f; }
            if (! juce::exactlyEqual (q, s.lastReso)) { s.filter.setResonance (q);       s.lastReso = q; }
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            s.filter.process (ctx);
        }
        else if (s.kind == Kind::convolution && s.convolution != nullptr)
        {
            const float mix = juce::jlimit (0.0f, 1.0f, s.mix.load());
            if (mix <= 0.0f)
                continue; // fully dry — skip the convolution entirely

            const float wetGain = juce::Decibels::decibelsToGain (s.wetGainDb.load());

            dryBuffer.makeCopyOf (buffer, true);

            juce::dsp::ProcessContextReplacing<float> ctx (block);
            s.convolution->process (ctx); // buffer is now fully wet

            // True dry/wet crossfade (mix=1 → fully wet, no dry). `wetGain` trims
            // the wet so it can be balanced against the dry (the normalised IR is
            // otherwise quieter than DecentSampler ran it).
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* wet = buffer.getWritePointer (ch);
                const auto* dry = dryBuffer.getReadPointer (juce::jmin (ch, dryBuffer.getNumChannels() - 1));
                for (int i = 0; i < numSamples; ++i)
                    wet[i] = dry[i] * (1.0f - mix) + (wet[i] * wetGain) * mix;
            }
        }
        else if (s.kind == Kind::gain)
        {
            buffer.applyGain (s.gainLinear.load());
        }
        else if (s.kind == Kind::chorus && s.chorusDelay != nullptr)
        {
            const float mix = juce::jlimit (0.0f, 1.0f, s.mix.load());
            if (mix <= 0.0f)
                continue;   // fully dry (e.g. mono switch) — skip

            const auto  sr        = (float) spec.sampleRate;
            const float centre    = juce::jmax (2.0f, s.chorusCentreMs) * 0.001f * sr;   // base delay (samples)
            const float modRange  = juce::jlimit (0.0f, centre - 1.0f, s.modDepth.load() * 0.010f * sr); // depth → ±(depth·10 ms)
            const float fb        = s.modFeedback.load();
            const double inc      = juce::MathConstants<double>::twoPi * (double) juce::jmax (0.01f, s.modRate.load()) / (double) sr;
            const int    lastCh   = juce::jmin (numCh, 2);   // widen the first two channels; extra channels pass dry

            // Hoist write pointers (getWritePointer per sample per channel adds up),
            // and compute the LFO once per sample — the right channel runs in exact
            // anti-phase, so sin(phase + π) is just -sin(phase).
            constexpr int kMaxCh = 16;
            const int nCh = juce::jmin (numCh, kMaxCh);
            float* wr[kMaxCh];
            for (int ch = 0; ch < nCh; ++ch)
                wr[ch] = buffer.getWritePointer (ch);

            for (int i = 0; i < numSamples; ++i)
            {
                const float lfoL = (float) std::sin (s.chorusPhase);
                for (int ch = 0; ch < lastCh; ++ch)
                {
                    // Left LFO at phase, right in anti-phase → their delays diverge → stereo width.
                    const float dry   = wr[ch][i];
                    const float lfo   = ch == 1 ? -lfoL : lfoL;
                    const float delay = juce::jlimit (1.0f, centre + modRange, centre + modRange * lfo);
                    s.chorusDelay->pushSample (ch, dry + fb * s.chorusLastWet[ch]);
                    const float wet = s.chorusDelay->popSample (ch, delay, true);
                    s.chorusLastWet[ch] = wet;
                    wr[ch][i] = dry * (1.0f - mix) + wet * mix;
                }
                for (int ch = lastCh; ch < nCh; ++ch)   // >2 channels: keep the line coherent, pass dry
                    s.chorusDelay->pushSample (ch, wr[ch][i]);
                s.chorusPhase += inc;
                if (s.chorusPhase >= juce::MathConstants<double>::twoPi)
                    s.chorusPhase -= juce::MathConstants<double>::twoPi;
            }
        }
        else if (s.kind == Kind::phaser && s.phaser != nullptr)
        {
            const float mix = juce::jlimit (0.0f, 1.0f, s.mix.load());
            if (mix <= 0.0f)
                continue;   // off (mix 0 / disabled) — skip
            const float rate  = juce::jmax (0.01f, s.modRate.load());
            const float depth = juce::jlimit (0.0f, 1.0f, s.modDepth.load());
            const float fb    = juce::jlimit (-0.95f, 0.95f, s.modFeedback.load());
            if (! juce::exactlyEqual (rate,  s.lastRate))  { s.phaser->setRate (rate);   s.lastRate  = rate; }
            if (! juce::exactlyEqual (depth, s.lastDepth)) { s.phaser->setDepth (depth); s.lastDepth = depth; }
            if (! juce::exactlyEqual (fb,    s.lastFb))    { s.phaser->setFeedback (fb); s.lastFb    = fb; }
            if (! juce::exactlyEqual (mix,   s.lastMix))   { s.phaser->setMix (mix);     s.lastMix   = mix; }
            juce::dsp::ProcessContextReplacing<float> ctx (block);
            s.phaser->process (ctx);
        }
        else if (s.kind == Kind::waveShaper)
        {
            // Soft saturation (DecentSampler-style): drive amplifies the input into a
            // tanh curve, output level is the makeup. tanh is ~linear for small signals
            // and rounds peaks smoothly, so a low drive stays nearly clean (e.g. the
            // Wurli "Japanese" amp at drive 4) and a high drive saturates progressively.
            const float drive  = juce::jmax (0.0001f, s.drive.load());
            const float outLvl = juce::jlimit (0.0f, 1.0f, s.output.load());   // DS outputLevel is 0..1
            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* w = buffer.getWritePointer (ch);
                for (int i = 0; i < numSamples; ++i)
                    w[i] = std::tanh (w[i] * drive) * outLvl;
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
    if      (parameter == "FX_FILTER_FREQUENCY") setEffectParam (index, FxParam::filterFrequency, value);
    else if (parameter == "FX_FILTER_RESONANCE") setEffectParam (index, FxParam::filterResonance, value);
    else if (parameter == "FX_MOD_RATE")         setEffectParam (index, FxParam::modRate, value);
    else if (parameter == "FX_MOD_DEPTH")        setEffectParam (index, FxParam::modDepth, value);
    else if (parameter == "FX_FEEDBACK")         setEffectParam (index, FxParam::feedback, value);
    else if (parameter == "FX_MIX")              setEffectParam (index, FxParam::mix, value);
    else if (parameter == "FX_DRIVE")            setEffectParam (index, FxParam::drive, value);
    else if (parameter == "LEVEL")               setEffectParam (index, FxParam::level, value);
    else if (parameter == "FX_OUTPUT_LEVEL")     setEffectParam (index, FxParam::outputLevel, value);
    else if (parameter == "ENABLED")             setEffectParam (index, FxParam::enabled, value);
}

void FxChain::setEffectParam (int index, FxParam parameter, float value) noexcept
{
    if (index < 0 || index >= (int) slots.size())
        return;

    auto& s = *slots[(size_t) index];
    switch (parameter)
    {
        case FxParam::filterFrequency: s.frequency.store (value);   break;
        case FxParam::filterResonance: s.resonance.store (value);   break;
        case FxParam::modRate:         s.modRate.store (value);     break;
        case FxParam::modDepth:        s.modDepth.store (value);    break;
        case FxParam::feedback:        s.modFeedback.store (value); break;
        case FxParam::mix:             s.mix.store (value);         break;
        case FxParam::drive:           s.drive.store (value);       break;
        case FxParam::level:           s.gainLinear.store (juce::Decibels::decibelsToGain (value)); break;   // gain effect, dB
        case FxParam::outputLevel:
            // Meaning depends on the slot kind: wave_shaper output (linear 0..1) vs
            // convolution wet trim (dB). We know the kind here, so route by it.
            (s.kind == Kind::waveShaper ? s.output : s.wetGainDb).store (value);
            break;
        case FxParam::enabled:         s.enabled.store (value > 0.5f); break;
    }
}

void FxChain::setEffectIr (int index, const SampleSource& source, const juce::String& irId)
{
    if (index < 0 || index >= (int) slots.size())
        return;
    auto& s = *slots[(size_t) index];
    if (s.kind != Kind::convolution || s.convolution == nullptr)
        return;

    if (const auto* ir = source.get (irId); ir != nullptr && ir->getNumFrames() > 0)
    {
        juce::AudioBuffer<float> irCopy;
        irCopy.makeCopyOf (ir->audio);
        s.convolution->loadImpulseResponse (
            std::move (irCopy), ir->sampleRate,
            ir->getNumChannels() > 1 ? juce::dsp::Convolution::Stereo::yes
                                     : juce::dsp::Convolution::Stereo::no,
            juce::dsp::Convolution::Trim::no,
            s.normalize ? juce::dsp::Convolution::Normalise::yes
                        : juce::dsp::Convolution::Normalise::no);
    }
}

void FxChain::setLowpassEnabled (bool enabled)
{
    for (auto& s : slots)
        if (s->kind == Kind::lowpass) { s->enabled.store (enabled); return; }
}

void FxChain::setLowpassFrequency (float hz)
{
    for (auto& s : slots)
        if (s->kind == Kind::lowpass) { s->frequency.store (hz); return; }
}

void FxChain::setReverbMix (float amount)
{
    for (auto& s : slots)
        if (s->kind == Kind::convolution) { s->mix.store (amount); return; }
}

void FxChain::setReverbWetGainDb (float db)
{
    for (auto& s : slots)
        if (s->kind == Kind::convolution) { s->wetGainDb.store (db); return; }
}

void FxChain::setGain (float db)
{
    for (auto& s : slots)
        if (s->kind == Kind::gain) { s->gainLinear.store (juce::Decibels::decibelsToGain (db)); return; }
}

void FxChain::setWaveShaperDrive (float drive)
{
    for (auto& s : slots)
        if (s->kind == Kind::waveShaper) { s->drive.store (drive); return; }
}

void FxChain::setWaveShaperOutput (float level)
{
    for (auto& s : slots)
        if (s->kind == Kind::waveShaper) { s->output.store (level); return; }
}

} // namespace dm
