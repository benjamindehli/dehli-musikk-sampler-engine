#include "SamplerEngine.h"
#include <cmath>

namespace dm
{

SamplerEngine::SamplerEngine()  = default;

SamplerEngine::~SamplerEngine()
{
    joinBuildThread();
    delete current.exchange (nullptr);
    delete pending.exchange (nullptr);
    drainRetired();
}

juce::String SamplerEngine::strumRateText() const
{
    if (auto* r = current.load())
        return r->sequencer.rateText();
    return {};
}

juce::String SamplerEngine::getVersion()
{
    return "dehli-musikk-sampler-engine 1.0.0";
}

SamplerEngine::ModeRender* SamplerEngine::buildMode (int index, std::atomic<float>* progress,
                                                    std::atomic<bool>* abort) const
{
    auto* mr = new ModeRender();
    mr->sequencer.prepare (sampleRate);
    mr->voices.prepare (sampleRate, maxBlockSize, numChannels);
    mr->fx.prepare (sampleRate, maxBlockSize, numChannels);

    if (library != nullptr && source != nullptr
        && index >= 0 && index < library->modes.size())
    {
        const auto& mode = library->modes.getReference (index);

        // Gather the distinct sample + IR ids this mode needs, then decode them (the heavy
        // part) reporting progress and honouring abort. Acquired PCM is released when the
        // ModeRender is destroyed, so only live modes cost RAM.
        mr->src = source;
        juce::StringArray ids;
        auto want = [&ids] (const juce::String& id)
        {
            if (id.isNotEmpty()) ids.addIfNotAlreadyThere (id);
        };
        for (const auto& g : mode.groups)
            for (const auto& s : g.samples)
                want (s.source);
        for (const auto& e : mode.effects)
            want (e.ir);
        for (const auto& tab : mode.ui.tabs)   // runtime-switchable IRs (cabinet / Home-Church)
        {
            for (const auto& menu : tab.menus)
                for (const auto& opt : menu.options)
                    for (const auto& b : opt.bindings)
                        if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                            want (b.translationValue.toString());
            for (const auto& btn : tab.buttons)
                for (const auto& st : btn.states)
                    for (const auto& b : st.bindings)
                        if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                            want (b.translationValue.toString());
        }

        // Register every id for release up front (single-threaded), then decode them in
        // PARALLEL — FLAC decode is CPU-bound and per-sample independent, and acquire()
        // now decodes outside its lock, so spreading the work across cores cuts the load
        // time roughly by the core count. Workers pull ids off a shared atomic index.
        for (const auto& id : ids)
            mr->held.add (id);

        const int total = juce::jmax (1, ids.size());
        std::atomic<int> nextIndex { 0 };
        std::atomic<int> completed { 0 };

        auto worker = [&]
        {
            for (int i = nextIndex.fetch_add (1); i < ids.size(); i = nextIndex.fetch_add (1))
            {
                if (abort != nullptr && abort->load())
                    return;
                mr->src->acquire (ids[i]);   // decode (or bump refcount if already resident)
                const int done = completed.fetch_add (1) + 1;
                if (progress != nullptr)
                    progress->store ((float) done / (float) total * 0.97f);   // headroom for wiring
            }
        };

        const unsigned hw = std::thread::hardware_concurrency();
        const int numThreads = juce::jlimit (1, 8, (int) (hw == 0 ? 4 : hw));
        std::vector<std::thread> pool;
        for (int t = 1; t < numThreads && t < ids.size(); ++t)
            pool.emplace_back (worker);
        worker();                       // the build thread decodes too
        for (auto& t : pool)
            t.join();

        if (abort != nullptr && abort->load())
            return mr;                  // caller deletes the partial unit

        mr->sequencer.configure (mode);
        // AirSupply config BEFORE setMode — setMode resolves which groups are the
        // air-driven reeds from the config's tags.
        if (library->airSupply)
            mr->voices.setAirSupply (*library->airSupply);
        mr->voices.setMode (mode, *source);   // samples now decoded → fast wiring
        mr->fx.setEffects (mode.effects, *source);

        // Re-apply any cabinet-menu IR selection so it survives a mode (re)build.
        for (int ei = 0; ei < kMaxEffects; ++ei)
            if (desiredIr[ei].isNotEmpty())
                mr->fx.setEffectIr (ei, *source, desiredIr[ei]);

        if (progress != nullptr)
            progress->store (1.0f);
    }
    return mr;
}

void SamplerEngine::joinBuildThread()
{
    if (buildThread.joinable())
    {
        abortBuild.store (true);
        buildThread.join();
    }
    abortBuild.store (false);
}

void SamplerEngine::drainRetired()
{
    // Take the whole retired stack in one exchange, then delete the chain. Message
    // thread only — deleting a ModeRender releases its held PCM back to the source.
    for (auto* p = retired.exchange (nullptr); p != nullptr;)
    {
        auto* next = p->nextRetired;
        delete p;
        p = next;
    }
}

void SamplerEngine::beginAsyncBuild (int index)
{
    // Message thread. Stop any in-flight build, free a retired unit, then decode the new
    // mode on a background thread and publish it via `pending` when done. Until then the
    // audio thread renders whatever `current` is (silence on first load).
    joinBuildThread();
    drainRetired();

    loadProgressValue.store (0.0f);
    loading.store (true);

    const int idx = index;
    buildThread = std::thread ([this, idx]
    {
        auto* mr = buildMode (idx, &loadProgressValue, &abortBuild);
        if (abortBuild.load())
        {
            delete mr;               // discarded — superseded/closing
        }
        else if (auto* superseded = pending.exchange (mr))
        {
            delete superseded;       // a previous pending was never adopted
        }
        loading.store (false);
    });
}

void SamplerEngine::prepare (double newSampleRate, int newMaxBlock, int newNumChannels)
{
    sampleRate   = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxBlockSize = juce::jmax (1, newMaxBlock);
    numChannels  = juce::jmax (1, newNumChannels);
    morphScratch.ensureStorageAllocated (128);   // chord-change morphs (audio thread; no realloc)

    // Rebuild the active mode for the new audio settings. Audio is stopped here, so we
    // can drop the stale unit outright; the new one decodes on the background thread
    // (the editor shows a progress overlay and we render silence until it publishes).
    if (library != nullptr)
    {
        joinBuildThread();
        delete current.exchange (nullptr);
        delete pending.exchange (nullptr);
        if (deferInitialBuild)
            deferredPending = true;   // wait for the mode chooser's pick
        else
            beginAsyncBuild (activeModeIndex);
    }
}

void SamplerEngine::setLibrary (const PresetLibrary& lib, SampleSource& src)
{
    library = &lib;
    source  = &src;
    libraryGain = juce::Decibels::decibelsToGain ((float) lib.gainDb);   // pre-FX level trim (--gain)
    activeModeIndex = juce::jlimit (0, juce::jmax (0, lib.modes.size() - 1), activeModeIndex);

    // Only build once prepared (prepare() builds otherwise — e.g. the constructor sets the
    // library before the host calls prepareToPlay).
    if (sampleRate > 0.0)
    {
        joinBuildThread();
        delete current.exchange (nullptr);
        delete pending.exchange (nullptr);
        if (deferInitialBuild)
            deferredPending = true;   // wait for the mode chooser's pick
        else
            beginAsyncBuild (activeModeIndex);
    }
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
    for (auto& g : ovGroupVelTrack)
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
    deferInitialBuild = false;   // a choice was made — build now and henceforth
    deferredPending   = false;

    // Each mode is independent: drop the previous mode's UI overrides so the new
    // one starts from its own manifest defaults (the editor re-applies its controls).
    resetOverrides();

    if (sampleRate <= 0.0)
        return; // not prepared yet; prepare() will build this mode

    // Decode + build the new mode on the background thread; the CURRENT mode keeps
    // playing until it's published (no silent gap on switch), then gets retired.
    beginAsyncBuild (index);
}

void SamplerEngine::applyFxOverrides (ModeRender& mr)
{
    // Audio thread — use the enum-addressed FxChain setters: the string overload would
    // construct a juce::String (heap alloc) per touched param per block, forever.
    using P = FxChain::FxParam;
    if (ovLowpassEnabled.touched.load()) mr.fx.setLowpassEnabled (ovLowpassEnabled.value.load() > 0.5f);
    for (int i = 0; i < kMaxEffects; ++i)
    {
        if (ovEffectEnabled[i].touched.load()) mr.fx.setEffectEnabled (i, ovEffectEnabled[i].value.load() > 0.5f);
        if (ovEffectMix[i].touched.load())     mr.fx.setEffectParam (i, P::mix,             ovEffectMix[i].value.load());
        if (ovEffectDrive[i].touched.load())   mr.fx.setEffectParam (i, P::drive,           ovEffectDrive[i].value.load());
        if (ovEffectLevel[i].touched.load())   mr.fx.setEffectParam (i, P::level,           ovEffectLevel[i].value.load());
        if (ovEffectOutput[i].touched.load())  mr.fx.setEffectParam (i, P::outputLevel,     ovEffectOutput[i].value.load());
        if (ovEffectFreq[i].touched.load())    mr.fx.setEffectParam (i, P::filterFrequency, ovEffectFreq[i].value.load());
        if (ovEffectReso[i].touched.load())      mr.fx.setEffectParam (i, P::filterResonance, ovEffectReso[i].value.load());
        if (ovEffectModRate[i].touched.load())   mr.fx.setEffectParam (i, P::modRate,  ovEffectModRate[i].value.load());
        if (ovEffectModDepth[i].touched.load())  mr.fx.setEffectParam (i, P::modDepth, ovEffectModDepth[i].value.load());
        if (ovEffectFeedback[i].touched.load())  mr.fx.setEffectParam (i, P::feedback, ovEffectFeedback[i].value.load());
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

    for (int i = 0; i < kMaxGroupVol; ++i)
        if (ovGroupVelTrack[i].touched.load())
            mr.voices.setGroupVelTrack (i, ovGroupVelTrack[i].value.load());
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midi,
                                  juce::AudioPlayHead* playHead)
{
    juce::ignoreUnused (playHead);   // M6 reads transport for the auto-strum sequencer

    // Adopt a newly-built mode, if one was published. Hand the old one back to the
    // message thread for deletion (never free on the audio thread) by PUSHING it onto
    // the retired stack — a plain store would drop (leak) a previously-retired mode
    // still waiting to be drained.
    if (auto* next = pending.exchange (nullptr))
    {
        if (auto* old = current.exchange (next))
        {
            old->nextRetired = retired.load (std::memory_order_relaxed);
            while (! retired.compare_exchange_weak (old->nextRetired, old))
            {}   // only drainRetired's exchange(nullptr) can race us; retry is cheap
        }
    }

    auto* cur = current.load (std::memory_order_relaxed);   // audio thread is the only writer
    if (cur == nullptr)
    {
        buffer.clear();
        return;
    }

    if (ovSequencerRateTouched.load())
        cur->sequencer.setRate (ovSequencerRate.load());
    if (ovSequencerRateNormTouched.load())
        cur->sequencer.setRateNorm (ovSequencerRateNorm.load());
    cur->sequencer.setTempoSync (seqTempoSync.load());
    cur->sequencer.setBpm (seqBpm.load());
    cur->sequencer.setBeatsPerStep (seqBeatsPerStep.load());
    if (ovSequencerIndexTouched.load())
        cur->sequencer.setIndexOffset (ovSequencerIndex.load());

    applyFxOverrides (*cur);
    cur->voices.setPitchBendRange (pitchBendUp.load(), pitchBendDown.load());
    cur->voices.setPitchDriftAmount (pitchDriftAmount.load());
    cur->voices.setVolumeDriftAmount (volumeDriftAmount.load());
    cur->voices.setSkipMutedGroups (skipMutedGroups.load());
    cur->voices.setRetriggerMute (retriggerMute.load());
    cur->voices.setAirSupplyEnabled (airSupplyEnabled.load());
    cur->voices.setMasterTune (masterTuneCents.load());
    cur->voices.setVelocityCurve (velocityCurve.load());
    if (const int cap = maxPolyphony.load(); cap > 0)
        cur->voices.setMaxPolyphony (cap);

    // Sequencer turns trigger keys into the strummed/played notes; non-trigger
    // keys pass straight through. In select+strum (Omnichord) mode a chord change
    // while strums ring emits morphs — apply them so the ringing voices glide to
    // the new chord's samples before this block renders.
    morphScratch.clearQuick();
    cur->sequencer.process (midi, sequencedMidi, buffer.getNumSamples(), &morphScratch);
    for (const auto& m : morphScratch)
        cur->voices.morphNote (m.from, m.to);
    cur->voices.processBlock (buffer, sequencedMidi);

    // Per-library trim (--gain) applied BEFORE the FX: DecentSampler reduces level
    // ahead of its effects, so the (input-level-dependent) wave_shaper sees the same
    // moderate signal DS feeds it — gentle saturation that grows as the mixer knobs
    // push harder, rather than a fully-clipped/collapsed signal. The master AMP_VOLUME
    // is an output control, applied AFTER the FX. (For a library with no active
    // wave_shaper the chain is linear, so pre- vs post-FX placement is equivalent.)
    if (! juce::exactlyEqual (libraryGain, 1.0f))
        buffer.applyGain (libraryGain);
    cur->fx.process (buffer);
    if (! juce::exactlyEqual (masterGain, 1.0f))
        buffer.applyGain (masterGain);
    if (! juce::exactlyEqual (uiMasterGain, 1.0f))   // user master output fader — the very last stage
        buffer.applyGain (uiMasterGain);

    // Output safety limiter. Summing many voices (this organ stacks 9 drawbars ×
    // double-track per note, then feeds the echo/reverb) can push well past full scale,
    // which the audio device hard-clips into loud pops. This is transparent below ±0.95
    // and smoothly asymptotes to ±1 above, so normal levels are untouched but overload
    // saturates gently instead of cracking.
    constexpr float kThresh = 0.95f, kRange = 1.0f - kThresh;
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0, n = buffer.getNumSamples(); i < n; ++i)
        {
            const float x = d[i];
            if      (x >  kThresh) d[i] =  kThresh + kRange * std::tanh ((x - kThresh) / kRange);
            else if (x < -kThresh) d[i] = -kThresh + kRange * std::tanh ((x + kThresh) / kRange);
        }
    }
}

void SamplerEngine::releaseResources()
{
    joinBuildThread();   // no build may outlive the audio settings it was built for
    if (auto* c = current.load())
        c->voices.releaseResources();
}

int SamplerEngine::getActiveVoiceCount() const noexcept
{
    auto* c = current.load();
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
    if      (parameter == "FX_MIX")              setEffectParam (effectIndex, FxChain::FxParam::mix, value);
    else if (parameter == "FX_DRIVE")            setEffectParam (effectIndex, FxChain::FxParam::drive, value);
    else if (parameter == "LEVEL")               setEffectParam (effectIndex, FxChain::FxParam::level, value);
    else if (parameter == "FX_OUTPUT_LEVEL")     setEffectParam (effectIndex, FxChain::FxParam::outputLevel, value);
    else if (parameter == "FX_FILTER_FREQUENCY") setEffectParam (effectIndex, FxChain::FxParam::filterFrequency, value);
    else if (parameter == "FX_FILTER_RESONANCE") setEffectParam (effectIndex, FxChain::FxParam::filterResonance, value);
    else if (parameter == "FX_MOD_RATE")         setEffectParam (effectIndex, FxChain::FxParam::modRate, value);
    else if (parameter == "FX_MOD_DEPTH")        setEffectParam (effectIndex, FxChain::FxParam::modDepth, value);
    else if (parameter == "FX_FEEDBACK")         setEffectParam (effectIndex, FxChain::FxParam::feedback, value);
    else if (parameter == "ENABLED")             setEffectParam (effectIndex, FxChain::FxParam::enabled, value);
}

void SamplerEngine::setEffectParam (int effectIndex, FxChain::FxParam parameter, float value) noexcept
{
    if (effectIndex < 0 || effectIndex >= kMaxEffects)
        return;
    using P = FxChain::FxParam;
    FxOverride* o = nullptr;
    switch (parameter)
    {
        case P::mix:             o = &ovEffectMix[effectIndex];      break;
        case P::drive:           o = &ovEffectDrive[effectIndex];    break;
        case P::level:           o = &ovEffectLevel[effectIndex];    break;
        case P::outputLevel:     o = &ovEffectOutput[effectIndex];   break;
        case P::filterFrequency: o = &ovEffectFreq[effectIndex];     break;
        case P::filterResonance: o = &ovEffectReso[effectIndex];     break;
        case P::modRate:         o = &ovEffectModRate[effectIndex];  break;
        case P::modDepth:        o = &ovEffectModDepth[effectIndex]; break;
        case P::feedback:        o = &ovEffectFeedback[effectIndex]; break;
        case P::enabled:         o = &ovEffectEnabled[effectIndex];  value = value > 0.5f ? 1.0f : 0.0f; break;
    }
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
    if (auto* c = current.load(); c != nullptr && source != nullptr)
        c->fx.setEffectIr (effectIndex, *source, irId);
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

void SamplerEngine::setSequencerRateNorm (double norm)
{
    ovSequencerRateNorm.store (norm);
    ovSequencerRateNormTouched.store (true);
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

void SamplerEngine::setGroupAmpVelTrack (int groupIndex, float amount)
{
    if (groupIndex >= 0 && groupIndex < kMaxGroupVol)
    {
        ovGroupVelTrack[groupIndex].value.store (amount);
        ovGroupVelTrack[groupIndex].touched.store (true);
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
    // Applied directly to the live mode's per-group chain. The compiled plan re-applies
    // these every block (idempotent), so after a mode swap the next block restores
    // the knob values over the freshly-built mode's manifest defaults.
    if (auto* c = current.load())
        c->voices.setGroupEffectParam (groupIndex, effectIndex, parameter, value);
}

void SamplerEngine::setGroupEffectParam (int groupIndex, int effectIndex,
                                         FxChain::FxParam parameter, float value)
{
    if (auto* c = current.load())
        c->voices.setGroupEffectParam (groupIndex, effectIndex, parameter, value);
}

void SamplerEngine::setGroupAmp (int groupIndex, const juce::String& parameter, float value)
{
    if      (parameter == "ENV_ATTACK")  setGroupAmpAttack  (groupIndex, value);
    else if (parameter == "ENV_DECAY")   setGroupAmpDecay   (groupIndex, value);
    else if (parameter == "ENV_SUSTAIN") setGroupAmpSustain (groupIndex, value);
    else if (parameter == "ENV_RELEASE") setGroupAmpRelease (groupIndex, value);
}

void SamplerEngine::setGroupAmpAttack  (int g, float s) { if (auto* c = current.load()) c->voices.setGroupAmpAttack  (g, s); }
void SamplerEngine::setGroupAmpDecay   (int g, float s) { if (auto* c = current.load()) c->voices.setGroupAmpDecay   (g, s); }
void SamplerEngine::setGroupAmpSustain (int g, float l) { if (auto* c = current.load()) c->voices.setGroupAmpSustain (g, l); }
void SamplerEngine::setGroupAmpRelease (int g, float s) { if (auto* c = current.load()) c->voices.setGroupAmpRelease (g, s); }

} // namespace dm
