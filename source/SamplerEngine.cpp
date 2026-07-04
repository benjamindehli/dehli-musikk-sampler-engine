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

        // Acquire (decode) this mode's samples + IRs before building; released when
        // the ModeRender is destroyed. So only the active mode's audio is in RAM.
        mr->src = source;
        auto hold = [mr] (const juce::String& id)
        {
            if (id.isNotEmpty() && ! mr->held.contains (id)) { mr->held.add (id); mr->src->acquire (id); }
        };
        for (const auto& g : mode.groups)
            for (const auto& s : g.samples)
                hold (s.source);
        for (const auto& e : mode.effects)
            hold (e.ir);
        for (const auto& tab : mode.ui.tabs)   // runtime-switchable IRs (cabinet / Home-Church)
        {
            for (const auto& menu : tab.menus)
                for (const auto& opt : menu.options)
                    for (const auto& b : opt.bindings)
                        if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                            hold (b.translationValue.toString());
            for (const auto& btn : tab.buttons)
                for (const auto& st : btn.states)
                    for (const auto& b : st.bindings)
                        if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                            hold (b.translationValue.toString());
        }

        mr->sequencer.configure (mode);
        mr->voices.setMode (mode, *source);
        mr->fx.setEffects (mode.effects, *source);

        // Re-apply any cabinet-menu IR selection so it survives a mode (re)build.
        for (int ei = 0; ei < kMaxEffects; ++ei)
            if (desiredIr[ei].isNotEmpty())
                mr->fx.setEffectIr (ei, *source, desiredIr[ei]);
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

void SamplerEngine::setLibrary (const PresetLibrary& lib, SampleSource& src)
{
    library = &lib;
    source  = &src;
    libraryGain = juce::Decibels::decibelsToGain ((float) lib.gainDb);   // pre-FX level trim (--gain)
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
                     &ovGain, &ovWaveDrive, &ovWaveOutput, &ovMasterVol,
                     &ovAmpAttack, &ovAmpDecay, &ovAmpSustain, &ovAmpRelease, &ovAmpVelTrack,
                     &ovAmpAttackCurve, &ovAmpDecayCurve, &ovAmpReleaseCurve })
        o->touched.store (false);
    for (int i = 0; i < kMaxMods; ++i)
    {
        ovLfoDepth[i].touched.store (false);
        ovLfoRate[i].touched.store (false);
    }
    for (auto& g : ovGroupVol)
        g.touched.store (false);
    for (auto& g : ovGroupTagVol)
        g.touched.store (false);
    for (auto& g : ovGroupGain)
        g.touched.store (false);
    for (auto& g : ovGroupEnabled)
        g.touched.store (false);
    for (auto& g : ovGroupTuning)
        g.touched.store (false);
    for (auto& g : ovGroupPan)
        g.touched.store (false);
    for (int i = 0; i < kMaxEffects; ++i)
    {
        ovEffectEnabled[i].touched.store (false);
        ovEffectMix[i].touched.store (false);
        ovEffectDrive[i].touched.store (false);
        ovEffectLevel[i].touched.store (false);
        ovEffectOutput[i].touched.store (false);
        ovEffectFreq[i].touched.store (false);
        ovEffectReso[i].touched.store (false);
        ovEffectModRate[i].touched.store (false);
        ovEffectModDepth[i].touched.store (false);
        ovEffectFeedback[i].touched.store (false);
    }
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
    {
        if (ovEffectEnabled[i].touched.load()) mr.fx.setEffectEnabled (i, ovEffectEnabled[i].value.load() > 0.5f);
        if (ovEffectMix[i].touched.load())     mr.fx.setEffectParam (i, "FX_MIX",          ovEffectMix[i].value.load());
        if (ovEffectDrive[i].touched.load())   mr.fx.setEffectParam (i, "FX_DRIVE",        ovEffectDrive[i].value.load());
        if (ovEffectLevel[i].touched.load())   mr.fx.setEffectParam (i, "LEVEL",           ovEffectLevel[i].value.load());
        if (ovEffectOutput[i].touched.load())  mr.fx.setEffectParam (i, "FX_OUTPUT_LEVEL", ovEffectOutput[i].value.load());
        if (ovEffectFreq[i].touched.load())    mr.fx.setEffectParam (i, "FX_FILTER_FREQUENCY", ovEffectFreq[i].value.load());
        if (ovEffectReso[i].touched.load())      mr.fx.setEffectParam (i, "FX_FILTER_RESONANCE", ovEffectReso[i].value.load());
        if (ovEffectModRate[i].touched.load())   mr.fx.setEffectParam (i, "FX_MOD_RATE",  ovEffectModRate[i].value.load());
        if (ovEffectModDepth[i].touched.load())  mr.fx.setEffectParam (i, "FX_MOD_DEPTH", ovEffectModDepth[i].value.load());
        if (ovEffectFeedback[i].touched.load())  mr.fx.setEffectParam (i, "FX_FEEDBACK",  ovEffectFeedback[i].value.load());
    }
    if (ovLowpassFreq.touched.load())    mr.fx.setLowpassFrequency (ovLowpassFreq.value.load());
    if (ovReverbMix.touched.load())      mr.fx.setReverbMix (ovReverbMix.value.load());
    if (ovReverbGain.touched.load())     mr.fx.setReverbWetGainDb (ovReverbGain.value.load());
    if (ovGain.touched.load())           mr.fx.setGain (ovGain.value.load());
    if (ovWaveDrive.touched.load())      mr.fx.setWaveShaperDrive (ovWaveDrive.value.load());
    if (ovWaveOutput.touched.load())     mr.fx.setWaveShaperOutput (ovWaveOutput.value.load());

    if (ovAmpAttack.touched.load())  mr.voices.setAmpAttack  (ovAmpAttack.value.load());
    if (ovAmpDecay.touched.load())   mr.voices.setAmpDecay   (ovAmpDecay.value.load());
    if (ovAmpSustain.touched.load()) mr.voices.setAmpSustain (ovAmpSustain.value.load());
    if (ovAmpRelease.touched.load()) mr.voices.setAmpRelease (ovAmpRelease.value.load());
    if (ovAmpAttackCurve.touched.load())  mr.voices.setAmpAttackCurve  (ovAmpAttackCurve.value.load());
    if (ovAmpDecayCurve.touched.load())   mr.voices.setAmpDecayCurve   (ovAmpDecayCurve.value.load());
    if (ovAmpReleaseCurve.touched.load()) mr.voices.setAmpReleaseCurve (ovAmpReleaseCurve.value.load());
    if (ovAmpVelTrack.touched.load()) mr.voices.setAmpVelTrack (ovAmpVelTrack.value.load());
    if (ovMasterVol.touched.load())   masterGain = ovMasterVol.value.load();   // applied post-FX in processBlock
    for (int i = 0; i < kMaxMods; ++i)
    {
        if (ovLfoDepth[i].touched.load()) mr.voices.setLfoDepth (i, ovLfoDepth[i].value.load());
        if (ovLfoRate[i].touched.load())  mr.voices.setLfoRate  (i, ovLfoRate[i].value.load());
    }

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupVol[i].touched.load())
            mr.voices.setGroupVolume (i, ovGroupVol[i].value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupTagVol[i].touched.load())
            mr.voices.setGroupTagVolume (i, ovGroupTagVol[i].value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupGain[i].touched.load())
            mr.voices.setGroupGain (i, ovGroupGain[i].value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupEnabled[i].touched.load())
            mr.voices.setGroupEnabled (i, ovGroupEnabled[i].value.load() > 0.5f);

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupTuning[i].touched.load())
            mr.voices.setGroupTuning (i, ovGroupTuning[i].value.load());

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupPan[i].touched.load())
            mr.voices.setGroupPan (i, ovGroupPan[i].value.load());
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
    current->voices.setPitchBendRange (pitchBendRange.load());
    current->voices.setPitchDriftAmount (pitchDriftAmount.load());
    current->voices.setVolumeDriftAmount (volumeDriftAmount.load());
    current->voices.setSkipMutedGroups (skipMutedGroups.load());

    // Sequencer turns trigger keys into the strummed/played notes; non-trigger
    // keys pass straight through.
    current->sequencer.process (midi, sequencedMidi, buffer.getNumSamples());
    current->voices.processBlock (buffer, sequencedMidi);

    // Per-library trim (--gain) applied BEFORE the FX: DecentSampler reduces level
    // ahead of its effects, so the (input-level-dependent) wave_shaper sees the same
    // moderate signal DS feeds it — gentle saturation that grows as the mixer knobs
    // push harder, rather than a fully-clipped/collapsed signal. The master AMP_VOLUME
    // is an output control, applied AFTER the FX. (For a library with no active
    // wave_shaper the chain is linear, so pre- vs post-FX placement is equivalent.)
    if (libraryGain != 1.0f)
        buffer.applyGain (libraryGain);
    current->fx.process (buffer);
    if (masterGain != 1.0f)
        buffer.applyGain (masterGain);
    if (uiMasterGain != 1.0f)   // user master output fader — the very last stage
        buffer.applyGain (uiMasterGain);
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

void SamplerEngine::setGain (float db)            { ovGain.value.store (db);          ovGain.touched.store (true); }
void SamplerEngine::setWaveShaperDrive (float d)  { ovWaveDrive.value.store (d);       ovWaveDrive.touched.store (true); }
void SamplerEngine::setWaveShaperOutput (float l) { ovWaveOutput.value.store (l);      ovWaveOutput.touched.store (true); }

void SamplerEngine::setEffectParam (int effectIndex, const juce::String& parameter, float value)
{
    if (effectIndex < 0 || effectIndex >= kMaxEffects)
        return;
    FxOverride* o = parameter == "FX_MIX"              ? &ovEffectMix[effectIndex]
                  : parameter == "FX_DRIVE"            ? &ovEffectDrive[effectIndex]
                  : parameter == "LEVEL"               ? &ovEffectLevel[effectIndex]
                  : parameter == "FX_OUTPUT_LEVEL"     ? &ovEffectOutput[effectIndex]
                  : parameter == "FX_FILTER_FREQUENCY" ? &ovEffectFreq[effectIndex]
                  : parameter == "FX_FILTER_RESONANCE" ? &ovEffectReso[effectIndex]
                  : parameter == "FX_MOD_RATE"         ? &ovEffectModRate[effectIndex]
                  : parameter == "FX_MOD_DEPTH"        ? &ovEffectModDepth[effectIndex]
                  : parameter == "FX_FEEDBACK"         ? &ovEffectFeedback[effectIndex]
                                                       : nullptr;
    if (o != nullptr) { o->value.store (value); o->touched.store (true); }
}

void SamplerEngine::setMasterVolume (float volume)
{
    ovMasterVol.value.store (volume);
    ovMasterVol.touched.store (true);
}

void SamplerEngine::setLfoDepth (int position, float depth)
{
    if (position < 0 || position >= kMaxMods) return;
    ovLfoDepth[position].value.store (depth);
    ovLfoDepth[position].touched.store (true);
}

void SamplerEngine::setLfoRate (int position, float hz)
{
    if (position < 0 || position >= kMaxMods) return;
    ovLfoRate[position].value.store (hz);
    ovLfoRate[position].touched.store (true);
}

void SamplerEngine::setEffectIr (int effectIndex, const juce::String& irId)
{
    // Message thread: remember the selection (so a (re)built mode picks it up in
    // buildMode) and load it into the live mode's FX now. The convolution swaps the
    // IR on its own background thread; `current` is only reassigned during a mode
    // switch, which also runs on the message thread, so it won't change under us.
    if (effectIndex < 0 || effectIndex >= kMaxEffects)
        return;
    desiredIr[effectIndex] = irId;
    if (current != nullptr && source != nullptr)
        current->fx.setEffectIr (effectIndex, *source, irId);
}

void SamplerEngine::setGroupTagVolume (int groupIndex, float volume)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupTagVol[groupIndex].value.store (volume);
        ovGroupTagVol[groupIndex].touched.store (true);
    }
}

void SamplerEngine::setGroupGain (int groupIndex, float db)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupGain[groupIndex].value.store (db);
        ovGroupGain[groupIndex].touched.store (true);
    }
}

void SamplerEngine::setGroupPan (int groupIndex, float pan)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupPan[groupIndex].value.store (pan);
        ovGroupPan[groupIndex].touched.store (true);
    }
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
void SamplerEngine::setAmpAttackCurve  (float c) { ovAmpAttackCurve.value.store (c);  ovAmpAttackCurve.touched.store (true); }
void SamplerEngine::setAmpDecayCurve   (float c) { ovAmpDecayCurve.value.store (c);   ovAmpDecayCurve.touched.store (true); }
void SamplerEngine::setAmpReleaseCurve (float c) { ovAmpReleaseCurve.value.store (c); ovAmpReleaseCurve.touched.store (true); }
void SamplerEngine::setAmpVelTrack (float a) { ovAmpVelTrack.value.store (a); ovAmpVelTrack.touched.store (true); }

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

void SamplerEngine::setGroupTuning (int groupIndex, float semitones)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupTuning[groupIndex].value.store (semitones);
        ovGroupTuning[groupIndex].touched.store (true);
    }
}

void SamplerEngine::setGroupEffectParam (int groupIndex, int effectIndex,
                                         const juce::String& parameter, float value)
{
    // Applied directly to the live mode's per-group chain. applyToEngine re-applies
    // these every block (idempotent), so after a mode swap the next block restores
    // the knob values over the freshly-built mode's manifest defaults.
    if (current != nullptr)
        current->voices.setGroupEffectParam (groupIndex, effectIndex, parameter, value);
}

void SamplerEngine::setGroupAmp (int groupIndex, const juce::String& parameter, float value)
{
    if (current == nullptr)
        return;
    auto& v = current->voices;
    if      (parameter == "ENV_ATTACK")  v.setGroupAmpAttack  (groupIndex, value);
    else if (parameter == "ENV_DECAY")   v.setGroupAmpDecay   (groupIndex, value);
    else if (parameter == "ENV_SUSTAIN") v.setGroupAmpSustain (groupIndex, value);
    else if (parameter == "ENV_RELEASE") v.setGroupAmpRelease (groupIndex, value);
}

} // namespace dm
