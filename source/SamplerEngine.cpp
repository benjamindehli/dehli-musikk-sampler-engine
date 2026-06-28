#include "SamplerEngine.h"

namespace dm
{

SamplerEngine::SamplerEngine()  = default;

SamplerEngine::~SamplerEngine()
{
    delete current;
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
}

juce::String SamplerEngine::getVersion()
{
    return "dehli-musikk-sampler-engine 0.5.0 (M5)";
}

SamplerEngine::ModeRender* SamplerEngine::buildMode (int index) const
{
    auto* mr = new ModeRender();
    mr->sequencer.prepare (sampleRate);
    mr->voices.prepare (sampleRate, maxBlockSize, numChannels);
    mr->fx.prepare (sampleRate, maxBlockSize, numChannels);

    if (library != nullptr && source != nullptr
        && index >= 0 && index < library->modes.size())
    {
        const auto& mode = library->modes.getReference (index);
        mr->sequencer.configure (mode);
        mr->voices.setMode (mode, *source);
        mr->fx.setEffects (mode.effects, *source);
    }
    return mr;
}

void SamplerEngine::setCurrentDirect (ModeRender* mr)
{
    // Audio is stopped here (prepareToPlay / setLibrary), so it's safe to replace
    // the live unit directly. Drain any queued units first.
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
    delete current;
    current = mr;
}

void SamplerEngine::prepare (double newSampleRate, int newMaxBlock, int newNumChannels)
{
    sampleRate   = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxBlockSize = juce::jmax (1, newMaxBlock);
    numChannels  = juce::jmax (1, newNumChannels);

    // Rebuild the active mode for the new audio settings (audio is stopped).
    if (library != nullptr)
        setCurrentDirect (buildMode (activeModeIndex));
}

void SamplerEngine::setLibrary (const PresetLibrary& lib, const SampleSource& src)
{
    library = &lib;
    source  = &src;
    activeModeIndex = juce::jlimit (0, juce::jmax (0, lib.modes.size() - 1), activeModeIndex);

    if (sampleRate > 0.0)
        setCurrentDirect (buildMode (activeModeIndex));
}

int SamplerEngine::getNumModes() const noexcept
{
    return library != nullptr ? library->modes.size() : 0;
}

juce::StringArray SamplerEngine::getModeNames() const
{
    juce::StringArray names;
    if (library != nullptr)
        for (const auto& m : library->modes)
            names.add (m.name);
    return names;
}

void SamplerEngine::resetOverrides()
{
    for (auto* o : { &ovLowpassEnabled, &ovLowpassFreq, &ovReverbMix, &ovReverbGain,
                     &ovAmpAttack, &ovAmpDecay, &ovAmpSustain, &ovAmpRelease })
        o->touched.store (false);
    for (auto& g : ovGroupVol)
        g.touched.store (false);
    for (auto& g : ovGroupEnabled)
        g.touched.store (false);
    for (auto& e : ovEffectEnabled)
        e.touched.store (false);
    ovSequencerRateTouched.store (false);
    ovSequencerIndexTouched.store (false);
}

void SamplerEngine::setActiveMode (int index)
{
    if (library == nullptr || index < 0 || index >= library->modes.size())
        return;

    activeModeIndex = index;

    // Each mode is independent: drop the previous mode's UI overrides so the new
    // one starts from its own manifest defaults (the editor re-applies its controls).
    resetOverrides();

    if (sampleRate <= 0.0)
        return; // not prepared yet; prepare() will build this mode

    auto* mr = buildMode (index);

    // Free a unit the audio thread already handed back.
    delete retired.exchange (nullptr);

    // Publish. If a previous pending was never adopted, it's ours to delete.
    if (auto* superseded = pending.exchange (mr))
        delete superseded;
}

void SamplerEngine::applyFxOverrides (ModeRender& mr)
{
    if (ovLowpassEnabled.touched.load()) mr.fx.setLowpassEnabled (ovLowpassEnabled.value.load() > 0.5f);
    for (int i = 0; i < kMaxEffects; ++i)
        if (ovEffectEnabled[i].touched.load())
            mr.fx.setEffectEnabled (i, ovEffectEnabled[i].value.load() > 0.5f);
    if (ovLowpassFreq.touched.load())    mr.fx.setLowpassFrequency (ovLowpassFreq.value.load());
    if (ovReverbMix.touched.load())      mr.fx.setReverbMix (ovReverbMix.value.load());
    if (ovReverbGain.touched.load())     mr.fx.setReverbWetGainDb (ovReverbGain.value.load());

    if (ovAmpAttack.touched.load())  mr.voices.setAmpAttack  (ovAmpAttack.value.load());
    if (ovAmpDecay.touched.load())   mr.voices.setAmpDecay   (ovAmpDecay.value.load());
    if (ovAmpSustain.touched.load()) mr.voices.setAmpSustain (ovAmpSustain.value.load());
    if (ovAmpRelease.touched.load()) mr.voices.setAmpRelease (ovAmpRelease.value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupVol[i].touched.load())
            mr.voices.setGroupVolume (i, ovGroupVol[i].value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupEnabled[i].touched.load())
            mr.voices.setGroupEnabled (i, ovGroupEnabled[i].value.load() > 0.5f);
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midi,
                                  juce::AudioPlayHead* playHead)
{
    juce::ignoreUnused (playHead);   // M6 reads transport for the auto-strum sequencer

    // Adopt a newly-built mode, if one was published. Hand the old one back to the
    // message thread for deletion (never free on the audio thread).
    if (auto* next = pending.exchange (nullptr))
    {
        auto* old = current;
        current = next;
        retired.store (old);   // old may be nullptr; that's fine
    }

    if (current == nullptr)
    {
        buffer.clear();
        return;
    }

    if (ovSequencerRateTouched.load())
        current->sequencer.setRate (ovSequencerRate.load());
    if (ovSequencerIndexTouched.load())
        current->sequencer.setIndexOffset (ovSequencerIndex.load());

    applyFxOverrides (*current);

    // Sequencer turns trigger keys into the strummed/played notes; non-trigger
    // keys pass straight through.
    current->sequencer.process (midi, sequencedMidi, buffer.getNumSamples());
    current->voices.processBlock (buffer, sequencedMidi);
    current->fx.process (buffer);
}

void SamplerEngine::releaseResources()
{
    if (current != nullptr)
        current->voices.releaseResources();
}

int SamplerEngine::getActiveVoiceCount() const noexcept
{
    auto* c = current;
    return c != nullptr ? c->voices.getActiveVoiceCount() : 0;
}

void SamplerEngine::setLowpassEnabled (bool enabled)
{
    ovLowpassEnabled.value.store (enabled ? 1.0f : 0.0f);
    ovLowpassEnabled.touched.store (true);
}

void SamplerEngine::setEffectEnabled (int index, bool enabled)
{
    if (index >= 0 && index < kMaxEffects)
    {
        ovEffectEnabled[index].value.store (enabled ? 1.0f : 0.0f);
        ovEffectEnabled[index].touched.store (true);
    }
}

void SamplerEngine::setLowpassFrequency (float hz)
{
    ovLowpassFreq.value.store (hz);
    ovLowpassFreq.touched.store (true);
}

void SamplerEngine::setReverbMix (float amount)
{
    ovReverbMix.value.store (amount);
    ovReverbMix.touched.store (true);
}

void SamplerEngine::setReverbWetGainDb (float db)
{
    ovReverbGain.value.store (db);
    ovReverbGain.touched.store (true);
}

void SamplerEngine::setSequencerRate (double stepsPerSecond)
{
    ovSequencerRate.store (stepsPerSecond);
    ovSequencerRateTouched.store (true);
}

void SamplerEngine::setSequencerIndexOffset (int offset)
{
    ovSequencerIndex.store (offset);
    ovSequencerIndexTouched.store (true);
}

void SamplerEngine::setAmpAttack  (float s) { ovAmpAttack.value.store (s);  ovAmpAttack.touched.store (true); }
void SamplerEngine::setAmpDecay   (float s) { ovAmpDecay.value.store (s);   ovAmpDecay.touched.store (true); }
void SamplerEngine::setAmpSustain (float l) { ovAmpSustain.value.store (l); ovAmpSustain.touched.store (true); }
void SamplerEngine::setAmpRelease (float s) { ovAmpRelease.value.store (s); ovAmpRelease.touched.store (true); }

void SamplerEngine::setGroupVolume (int groupIndex, float volume)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupVol[groupIndex].value.store (volume);
        ovGroupVol[groupIndex].touched.store (true);
    }
}

void SamplerEngine::setGroupEnabled (int groupIndex, bool enabled)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupEnabled[groupIndex].value.store (enabled ? 1.0f : 0.0f);
        ovGroupEnabled[groupIndex].touched.store (true);
    }
}

} // namespace dm
