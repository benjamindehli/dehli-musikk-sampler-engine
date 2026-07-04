#include "VoiceEngine.h"
#include <cmath>

namespace dm
{

namespace
{
constexpr int kMaxVoices = 64;   // headroom for multi-group layering (drums stack several layers per hit)

// Modulator waveform shape name → index, and the shape's value at a phase (cycles) → -1..1.
int lfoShapeFromString (const juce::String& s)
{
    if (s == "square")                 return 3;
    if (s == "saw" || s == "sawtooth") return 2;
    if (s == "triangle")               return 1;
    return 0;   // sine (default)
}

float lfoWave (int shape, double phaseCycles)
{
    const double ph = phaseCycles - std::floor (phaseCycles);   // 0..1
    switch (shape)
    {
        case 1: return (float) (1.0 - 4.0 * std::abs (ph - 0.5));   // triangle
        case 2: return (float) (2.0 * ph - 1.0);                    // saw (ramp up)
        case 3: return ph < 0.5 ? 1.0f : -1.0f;                     // square
        default: return (float) std::sin (juce::MathConstants<double>::twoPi * ph);
    }
}

// Resolve the effective amp envelope for a group: mode defaults, with the group's
// own overrides applied where present.
CurvedAdsr::Parameters resolveAdsr (const AmpEnvelope& amp, const Group& g)
{
    CurvedAdsr::Parameters p;
    p.attack  = (float) (g.attack  ? *g.attack  : amp.attack);
    p.decay   = (float) (g.decay   ? *g.decay   : amp.decay);
    p.sustain = (float) (g.sustain ? *g.sustain : amp.sustain);
    p.release = (float) (g.release ? *g.release : amp.release);
    // Curve shape: manifest value if present, else DecentSampler defaults
    // (CurvedAdsr::Parameters already defaults to attack -100, decay/release +100).
    if (amp.attackCurve)  p.attackCurve  = (float) *amp.attackCurve;
    if (amp.decayCurve)   p.decayCurve   = (float) *amp.decayCurve;
    if (amp.releaseCurve) p.releaseCurve = (float) *amp.releaseCurve;
    return p;
}

// Windowed-sinc (Lanczos) fractional-position resampling. A precomputed kernel
// table (kSincTaps taps × kSincPhases sub-sample phases) gives clean interpolation
// for pitch-shifted / sample-rate-mismatched playback; at integer positions it
// reduces to the exact sample (fast path), so 48k-in-48k playback is untouched.
namespace
{
constexpr int kSincTaps   = 8;             // 4 taps either side
constexpr int kSincHalf   = kSincTaps / 2;
constexpr int kSincPhases = 512;           // sub-sample phase resolution

struct SincTable
{
    float k[kSincPhases + 1][kSincTaps];

    SincTable()
    {
        const double pi = juce::MathConstants<double>::pi;
        auto sinc = [pi] (double x) { return x == 0.0 ? 1.0 : std::sin (pi * x) / (pi * x); };
        const double a = (double) kSincHalf;   // Lanczos window half-width

        for (int p = 0; p <= kSincPhases; ++p)
        {
            const double frac = (double) p / (double) kSincPhases;
            double row[kSincTaps], sum = 0.0;
            for (int t = 0; t < kSincTaps; ++t)
            {
                const double x = (double) (t - (kSincHalf - 1)) - frac;
                const double w = std::abs (x) < a ? sinc (x) * sinc (x / a) : 0.0;
                row[t] = w;
                sum   += w;
            }
            for (int t = 0; t < kSincTaps; ++t)   // normalise for unity DC gain
                k[p][t] = (float) (sum != 0.0 ? row[t] / sum : 0.0);
        }
    }
};

const SincTable& sincTable() { static const SincTable t; return t; }
}

float interpolate (const juce::AudioBuffer<float>& buf, int ch, double pos)
{
    const int n  = buf.getNumSamples();
    const int i0 = (int) pos;
    if (i0 < 0 || i0 >= n)
        return 0.0f;

    const double frac = pos - (double) i0;
    const float* data = buf.getReadPointer (ch);
    const int p = (int) (frac * (double) kSincPhases + 0.5);
    if (p == 0)
        return data[i0];   // integer position → exact sample (no interpolation)

    const auto* kr = sincTable().k[p];
    const int base = i0 - (kSincHalf - 1);
    float sum = 0.0f;
    for (int t = 0; t < kSincTaps; ++t)
    {
        const int idx = base + t;
        if (idx >= 0 && idx < n)
            sum += data[idx] * kr[t];
    }
    return sum;
}

// Loop read with an equal-gain crossfade across the last `xf` frames before
// loopEnd: blends the approach-to-loopEnd with the matching material before
// loopStart (pos - loopLen), so the wrap is seamless.
float readLooped (const juce::AudioBuffer<float>& buf, int ch, double pos,
                  double loopEnd, double loopLen, double xf)
{
    const float base = interpolate (buf, ch, pos);
    if (xf > 0.0 && pos > loopEnd - xf)
    {
        const double t = (pos - (loopEnd - xf)) / xf;        // 0 → 1 across the fade
        const float prev = interpolate (buf, ch, pos - loopLen);
        return (float) ((1.0 - t) * base + t * prev);
    }
    return base;
}
} // namespace

VoiceEngine::VoiceEngine()
{
    voices.resize (kMaxVoices);
}

void VoiceEngine::prepare (double newSampleRate, int maxBlockSize, int numChannels)
{
    sincTable();   // build the interpolation kernel now (message thread), not on first audio callback
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    maxBlock   = juce::jmax (1, maxBlockSize);
    numChans   = juce::jmax (1, numChannels);
    instTrem.assign ((size_t) maxBlock, 1.0f);
    for (auto& v : voices)
    {
        v.active = false;
        v.adsr.setSampleRate (sampleRate);
    }
    for (auto& gc : groupChains)
        if (gc) gc->prepare (sampleRate, maxBlock, numChans);
    for (auto& gb : groupBuffers)
        gb.setSize (numChans, maxBlock, false, false, true);
}

void VoiceEngine::releaseResources()
{
    allNotesOff();
}

void VoiceEngine::setMode (const Mode& mode, const SampleSource& source)
{
    allNotesOff();
    zones.clearQuick();
    hasDriftGateButton = false;   // set below if any sample carries a per-sample drift marker

    const int numGroups = mode.groups.size();
    rrCounter.clearQuick(); rrCounter.insertMultiple (0, 0, numGroups);
    rrLast.clearQuick();    rrLast.insertMultiple (0, -1, numGroups);
    rrRandom.setSeed (20240101);   // fixed → reproducible variation across sessions
    groupVolume.clearQuick();    groupVolume.insertMultiple (0, 1.0f, numGroups);
    groupTagVolume.clearQuick(); groupTagVolume.insertMultiple (0, 1.0f, numGroups);
    groupGain.clearQuick();      groupGain.insertMultiple (0, 1.0f, numGroups);
    groupPan.clearQuick();       groupPan.insertMultiple (0, 0.0f, numGroups);   // 0 = centred
    groupEnabled.clearQuick();   groupEnabled.insertMultiple (0, true, numGroups);
    groupReleaseTrigger.clearQuick(); groupReleaseTrigger.insertMultiple (0, false, numGroups);
    groupCcLo.clearQuick();  groupCcLo.insertMultiple (0, -1, numGroups);
    groupCcHi.clearQuick();  groupCcHi.insertMultiple (0, -1, numGroups);
    groupAttack.clearQuick();  groupAttack.insertMultiple  (0, -1.0f, numGroups);
    groupDecay.clearQuick();   groupDecay.insertMultiple   (0, -1.0f, numGroups);
    groupSustain.clearQuick(); groupSustain.insertMultiple (0, -1.0f, numGroups);
    groupRelease.clearQuick(); groupRelease.insertMultiple (0, -1.0f, numGroups);
    groupTuningMul.clearQuick(); groupTuningMul.insertMultiple (0, 1.0, numGroups);
    sustainValue = 0; sustainActive = false;
    for (auto& nv : noteOnVelocity) nv = 0.8f;   // fallback for a note-off with no prior note-on

    // Tags with polyphony 1 make their groups monophonic. Reuse the choke
    // mechanism: add such a group's own mono tags to its silencedByTags.
    juce::StringArray monoTags;
    for (const auto& t : mode.tags)
        if (t.polyphony && *t.polyphony <= 1)
            monoTags.add (t.name);

    for (int gi = 0; gi < numGroups; ++gi)
    {
        const auto& g = mode.groups.getReference (gi);

        Zone proto;
        proto.adsr     = resolveAdsr (mode.amp, g);
        // ampEnvEnabled=false → the amp envelope is bypassed and the sample rings to
        // its natural end ignoring note-off (a one-shot, e.g. an un-damped/sustained group).
        proto.ampEnv   = g.ampEnvEnabled.value_or (mode.amp.enabled);
        proto.releaseTrigger = (g.trigger == "release");
        groupReleaseTrigger.set (gi, proto.releaseTrigger);
        proto.gain     = (float) (g.volume   ? *g.volume   : mode.amp.volume);
        proto.velTrack = (float) (g.velTrack ? *g.velTrack : mode.amp.velTrack);
        proto.groupIndex = gi;
        if (g.velocity) { proto.loVel = g.velocity->lo; proto.hiVel = g.velocity->hi; }
        proto.tags = g.tags;
        if (g.silencing)
            proto.silencedByTags = g.silencing->byTags;
        for (const auto& tg : g.tags)               // tag-polyphony 1 → self-choke (mono)
            if (monoTags.contains (tg) && ! proto.silencedByTags.contains (tg))
                proto.silencedByTags.add (tg);
        if (g.roundRobin)
        {
            proto.roundRobin = true;
            proto.rrMode = g.roundRobin->mode;
        }
        const bool groupPitchKeyTrack = g.pitchKeyTrack ? *g.pitchKeyTrack : false;

        for (const auto& s : g.samples)
        {
            const auto* buffer = source.get (s.source);
            if (buffer == nullptr || buffer->getNumFrames() <= 0)
                continue; // unresolved/empty sample — skip rather than crash

            if (s.onLoCC64 && s.onHiCC64)   // sustain-pedal-triggered group (damper noise)
            {
                groupCcLo.set (gi, *s.onLoCC64);
                groupCcHi.set (gi, *s.onHiCC64);
            }

            Zone z = proto;
            z.loNote = s.loNote;
            z.hiNote = s.hiNote;
            z.rootNote = s.rootNote;
            z.buffer = buffer;
            z.pitchKeyTrack = s.pitchKeyTrack || groupPitchKeyTrack;

            // A per-sample drift marker (fractional pitchKeyTrack in the DS source, e.g.
            // Strykebrett) means this library has a dedicated Drift on/off button wired to
            // a GLOBAL_TUNING modulator. We no longer use the value for depth (that's random
            // per voice now), only as the signal that the drift wheels should be GATED by
            // that button. Libraries without it leave drift always-on (wheel = 0 disables).
            if (s.pitchDrift)
                hasDriftGateButton = true;

            // Loop, validated against the actual decoded length — out-of-range
            // points (e.g. authored against a stale .dspreset length) fall back to
            // one-shot rather than reading past the buffer.
            if (s.loop.enabled && s.loop.start && s.loop.end)
            {
                const int frames = buffer->getNumFrames();
                const int st = *s.loop.start;
                const int en = *s.loop.end;
                if (st >= 0 && en > st && en <= frames)
                {
                    z.loopEnabled = true;
                    z.loopStart = st;
                    z.loopEnd   = en;
                    z.loopLen   = en - st;
                    z.loopXf    = juce::jlimit (0, juce::jmin (z.loopLen, st),
                                                s.loop.crossfade.value_or (0));
                }
            }

            zones.add (z);
        }
    }

    // Per-group insert FX chains (e.g. organ swell filter + loudness). Built once
    // per group that declares effects; rendered per-group in processBlock.
    groupChains.clear();
    groupBuffers.clear();
    groupChains.resize ((size_t) numGroups);
    anyGroupFx = false;
    for (int gi = 0; gi < numGroups; ++gi)
    {
        const auto& g = mode.groups.getReference (gi);
        if (g.effects.isEmpty())
            continue;
        auto chain = std::make_unique<FxChain>();
        chain->prepare (sampleRate, maxBlock, numChans);
        chain->setEffects (g.effects, source);
        groupChains[(size_t) gi] = std::move (chain);
        anyGroupFx = true;
    }
    if (anyGroupFx)
    {
        groupBuffers.resize ((size_t) numGroups);
        for (auto& gb : groupBuffers)
            gb.setSize (numChans, maxBlock, false, false, true);
    }

    // LFO (first modulator). Two kinds of target are supported:
    //  - AMP_VOLUME on a group → smooth per-sample amplitude tremolo (Wurli).
    //  - effect params / GLOBAL_TUNING → applied per block in applyLfoBlock (elektrisk
    //    tremulant: gain + filter sweep + slight pitch vibrato).
    // Depth = MOD_AMOUNT control, rate = FREQUENCY control (both override the defaults).
    // Build every modulator (not just the first). Each one's depth starts at its
    // manifest modAmount and can be overridden per-position by a MOD_AMOUNT control.
    mods.clear();
    hasInstMod = false;
    globalTuningMul = 1.0;
    groupHasTrem.clearQuick();
    groupHasTrem.insertMultiple (0, false, numGroups);
    groupTuningModMul.clearQuick();
    groupTuningModMul.insertMultiple (0, 1.0, numGroups);

    for (const auto& lfo : mode.modulators)
    {
        Modulator m;
        m.freqHz   = lfo.frequency;
        m.depth    = (float) lfo.modAmount;
        m.shape    = lfoShapeFromString (lfo.shape);
        m.bindings = lfo.bindings;
        // Resolve id-based group targets → indices ONCE here, so the audio thread
        // (applyLfoBlock) reads plain group indices. targetId is a group uid during the
        // migration; fall back to the binding's own groupIndex when absent.
        for (auto& b : m.bindings)
            if (b.targetId.isNotEmpty())
                for (int gi = 0; gi < mode.groups.size(); ++gi)
                    if (mode.groups.getReference (gi).uid == b.targetId) { b.groupIndex = gi; break; }

        for (const auto& b : m.bindings)
            if (b.parameter == "AMP_VOLUME")
            {
                if (b.groupIndex)
                {
                    const int gi = *b.groupIndex;
                    if (gi >= 0 && gi < numGroups)
                    {
                        m.ampGroups.addIfNotAlreadyThere (gi);
                        groupHasTrem.set (gi, true);
                    }
                }
                else
                {
                    m.ampInstrument = true;
                    hasInstMod = true;
                }
            }
        mods.push_back (std::move (m));
    }
    // Per-group tremolo scratch (each sized to a block; only trem groups are filled/read).
    groupTrem.assign ((size_t) numGroups, std::vector<float> ((size_t) maxBlock, 1.0f));
}

const VoiceEngine::Zone* VoiceEngine::pickZoneInGroup (int group, int note, int velocity)
{
    auto covers = [note, velocity] (const Zone& z)
    {
        return note >= z.loNote && note <= z.hiNote
            && velocity >= z.loVel && velocity <= z.hiVel;   // velocity layer
    };

    // This group's zones covering the note+velocity.
    juce::Array<int> candidates;
    bool roundRobin = false;
    juce::String rrMode;
    for (int i = 0; i < zones.size(); ++i)
    {
        const auto& z = zones.getReference (i);
        if (z.groupIndex == group && covers (z))
        {
            candidates.add (i);
            roundRobin = z.roundRobin;
            rrMode     = z.rrMode;
        }
    }
    if (candidates.isEmpty())
        return nullptr;
    if (candidates.size() == 1 || ! roundRobin)
        return &zones.getReference (candidates[0]);

    int pick = 0;
    if (rrMode == "round_robin" || rrMode == "round-robin")
    {
        const int c = (group >= 0 && group < rrCounter.size()) ? rrCounter[group] : 0;
        pick = c % candidates.size();
        if (group >= 0 && group < rrCounter.size())
            rrCounter.set (group, c + 1);
    }
    else // "random" (and any unknown mode)
    {
        pick = rrRandom.nextInt (candidates.size());
        if (group >= 0 && group < rrLast.size() && pick == rrLast[group])
            pick = (pick + 1) % candidates.size();   // avoid immediate repeat
    }

    if (group >= 0 && group < rrLast.size())
        rrLast.set (group, pick);

    return &zones.getReference (candidates[pick]);
}

VoiceEngine::Voice* VoiceEngine::allocateVoice()
{
    for (auto& v : voices)
        if (! v.active)
            return &v;

    // Steal the oldest active voice.
    Voice* oldest = &voices.front();
    for (auto& v : voices)
        if (v.startOrder < oldest->startOrder)
            oldest = &v;
    return oldest;
}

void VoiceEngine::handleNoteOn (int note, float velocity)
{
    const int velMidi = juce::jlimit (0, 127, juce::roundToInt (velocity * 127.0f));
    if (note >= 0 && note < 128)
        noteOnVelocity[note] = velocity;   // remembered for the release-trigger groups

    // Start a voice for EVERY enabled attack group covering this note+velocity —
    // drums layer several groups on one note (and velocity layers are separate
    // groups); round-robin picks within each group. Release groups fire on note-off.
    const int numGroups = groupEnabled.size();
    for (int g = 0; g < numGroups; ++g)
    {
        if (! groupEnabled[g] || groupReleaseTrigger[g])
            continue;
        // Skip groups muted to true silence — e.g. a drawbar pulled fully down. There's
        // no point spawning a voice that contributes nothing, and it frees polyphony for
        // the drawbars that ARE up (a big win on this organ's 9 drawbars × double-track).
        // Only skip at ~zero, so a barely-open drawbar still sounds. User-toggleable: with
        // the skip off, every group triggers (so raising a drawbar mid-note brings it in).
        if (skipMutedGroups && groupVolume[g] * groupTagVolume[g] * groupGain[g] <= 1.0e-6f)
            continue;
        if (const auto* zone = pickZoneInGroup (g, note, velMidi))
            startVoice (*zone, note, velocity);
    }
}

void VoiceEngine::startVoice (const Zone& zone, int note, float velocity)
{
    const int fadeOutSamples = juce::jmax (1, (int) (sampleRate * 0.004)); // ~4 ms declick

    // Tag choke: fade out (don't hard-cut) any active voice this voice silences,
    // so a fast mono retrigger doesn't click. The choked voice keeps rendering in
    // its own slot until the ramp reaches zero.
    if (! zone.tags.isEmpty())
    {
        for (auto& v : voices)
        {
            if (! v.active || v.fadingOut)
                continue;
            for (const auto& t : zone.tags)
                if (v.silencedByTags.contains (t))
                {
                    v.fadingOut = true;
                    v.declickDelta = -1.0f / (float) fadeOutSamples;
                    break;
                }
        }
    }

    Voice* v = allocateVoice();
    v->active = true;
    v->buffer = zone.buffer;
    v->position = 0.0;
    v->note = note;
    v->isRelease = zone.releaseTrigger;
    v->groupIndex = zone.groupIndex;
    v->tags = zone.tags;
    v->silencedByTags = zone.silencedByTags;
    // Independent per-voice drift: random depth (0.4..1) + random phase for both the pitch
    // and volume drift LFOs, so every note wanders on its own.
    v->pitchDriftDepth = 0.4f + 0.6f * driftRandom.nextFloat();
    v->volDriftDepth   = 0.4f + 0.6f * driftRandom.nextFloat();
    v->driftPhase      = driftRandom.nextDouble() * juce::MathConstants<double>::twoPi;
    v->volDriftPhase   = driftRandom.nextDouble() * juce::MathConstants<double>::twoPi;

    double rate = zone.buffer->sampleRate / sampleRate;
    if (zone.pitchKeyTrack)
        rate *= std::pow (2.0, (note - zone.rootNote) / 12.0);
    if (zone.groupIndex >= 0 && zone.groupIndex < groupTuningMul.size())
        rate *= groupTuningMul[zone.groupIndex];   // GROUP_TUNING
    v->rate = rate;

    v->loopEnabled = zone.loopEnabled;
    v->loopEnd = (double) zone.loopEnd;
    v->loopLen = (double) zone.loopLen;
    v->loopXf  = (double) zone.loopXf;

    const float vt = (ovVelTrack >= 0.0f) ? ovVelTrack : zone.velTrack;   // global sensitivity override
    const float velGain = 1.0f - vt + vt * velocity;
    v->gain = zone.gain * velGain;

    v->ampEnv = zone.ampEnv;
    v->baseAdsr = zone.adsr;
    v->adsr.setSampleRate (sampleRate);
    v->adsr.setParameters (effectiveAdsr (v->baseAdsr, zone.groupIndex));
    v->adsr.noteOn();
    v->startOrder = ++orderCounter;

    // No fade-in — the sample's own start handles the onset. The declick ramp is
    // only used for the choke/steal fade-out below.
    v->fadingOut = false;
    v->declickGain = 1.0f;
    v->declickDelta = 0.0f;
}

void VoiceEngine::handleNoteOff (int note)
{
    // Release held (attack) voices — unless the sustain pedal is down, in which case
    // mark them held so they keep ringing until the pedal lifts. Release-trigger
    // (key-off) voices are one-shots that ignore note-off and play out.
    for (auto& v : voices)
        if (v.active && v.note == note && ! v.isRelease && v.ampEnv)   // one-shots (ampEnv=false) ignore note-off
        {
            if (sustainActive) v.pedalHeld = true;
            else               v.adsr.noteOff();
        }

    // Fire release-trigger groups for this note, at the velocity it was played.
    const float velocity = (note >= 0 && note < 128) ? noteOnVelocity[note] : 0.8f;
    const int velMidi = juce::jlimit (0, 127, juce::roundToInt (velocity * 127.0f));
    const int numGroups = groupReleaseTrigger.size();
    for (int g = 0; g < numGroups; ++g)
    {
        if (! groupEnabled[g] || ! groupReleaseTrigger[g])
            continue;
        if (const auto* zone = pickZoneInGroup (g, note, velMidi))
            startVoice (*zone, note, velocity);
    }
}

void VoiceEngine::handleSustain (int value)
{
    // Damper noise: fire any pedal-triggered group whose CC range the pedal just
    // entered (e.g. damper-lift on press, damper-drop on release). These groups have
    // loNote=-1, so pickZoneInGroup(-1) selects/round-robins among their samples; the
    // voice plays out as a one-shot (note=-1 → never matched by a key note-off), and
    // damper-drop chokes damper-lift via the normal tag silencing.
    auto inRange = [] (int v, int lo, int hi) { return v >= lo && v <= hi; };
    for (int g = 0; g < groupCcLo.size(); ++g)
    {
        if (groupCcLo[g] < 0 || ! groupEnabled[g])
            continue;
        const bool was = inRange (sustainValue, groupCcLo[g], groupCcHi[g]);
        const bool now = inRange (value,        groupCcLo[g], groupCcHi[g]);
        if (now && ! was)
            if (const auto* zone = pickZoneInGroup (g, -1, 127))
                startVoice (*zone, -1, 1.0f);
    }

    // Sustain hold: pedal up releases everything held while it was down.
    const bool down = value >= 64;
    if (sustainActive && ! down)
        for (auto& v : voices)
            if (v.active && v.pedalHeld)
            {
                v.adsr.noteOff();
                v.pedalHeld = false;
            }
    sustainActive = down;
    sustainValue  = value;
}

void VoiceEngine::handlePitchWheel (int wheelValue)
{
    // Centre (8192) → no bend → multiplier 1.0. Range is configurable (default ±2).
    const double semis = (wheelValue - 8192) / 8192.0 * bendRangeSemitones;
    pitchBendMul = std::pow (2.0, semis / 12.0);
}

void VoiceEngine::renderChunk (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;

    const int outChannels = buffer.getNumChannels();

    // Amplitude tremolo + pitch modulation are computed once per block in
    // applyLfoBlock and indexed here by absolute block position (startSample + i):
    // instTrem = instrument-level tremolo (all voices), groupTrem[g] = per-group.
    const bool instTremActive = hasInstMod && (int) instTrem.size() >= startSample + numSamples;

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const int srcChannels = v.buffer->getNumChannels();
        const int frames = v.buffer->getNumFrames();
        const bool groupTremActive = v.groupIndex >= 0 && v.groupIndex < groupHasTrem.size()
                                  && groupHasTrem[v.groupIndex]
                                  && (int) groupTrem[(size_t) v.groupIndex].size() >= startSample + numSamples;
        // Combined pitch multiplier for this voice: global vibrato × its group's vibrato.
        double tuneMul = globalTuningMul;
        if (v.groupIndex >= 0 && v.groupIndex < groupTuningModMul.size())
            tuneMul *= groupTuningModMul[v.groupIndex];

        // Independent per-voice drift (all plugins), block-rate (drift is <1 Hz). Each voice
        // has a random depth + phase (see startVoice), so held notes wander on their own.
        // The two right-side wheels set the amounts. kMax* are the swing at wheel=1, depth=1.
        const auto twoPi = juce::MathConstants<double>::twoPi;
        if (const float pDriftAmt = driftGateOpen ? pitchDriftAmount.load() : 0.0f; pDriftAmt > 0.0f)
        {
            constexpr double kMaxDriftSemis = 0.15;   // ± semitones (reduced)
            const double driftSemis = std::sin (v.driftPhase) * (double) pDriftAmt * (double) v.pitchDriftDepth * kMaxDriftSemis;
            tuneMul *= std::pow (2.0, driftSemis / 12.0);
            v.driftPhase += twoPi * driftRateHz * (double) numSamples / sampleRate;
            if (v.driftPhase >= twoPi) v.driftPhase = std::fmod (v.driftPhase, twoPi);
        }

        // Volume drift → a per-block gain multiplier folded into `g` in the sample loop.
        float volDriftMul = 1.0f;
        if (const float vDriftAmt = driftGateOpen ? volumeDriftAmount.load() : 0.0f; vDriftAmt > 0.0f)
        {
            constexpr float kMaxVolDrift = 0.2f;   // ±20% gain at wheel=1, depth=1
            volDriftMul = 1.0f + (float) std::sin (v.volDriftPhase) * vDriftAmt * v.volDriftDepth * kMaxVolDrift;
            v.volDriftPhase += twoPi * volDriftRateHz * (double) numSamples / sampleRate;
            if (v.volDriftPhase >= twoPi) v.volDriftPhase = std::fmod (v.volDriftPhase, twoPi);
        }

        // Voices in a group with its own FX chain render into that group's scratch
        // buffer (filtered + gained as a group post-loop); others go straight out.
        juce::AudioBuffer<float>* target = &buffer;
        if (anyGroupFx && v.groupIndex >= 0 && (size_t) v.groupIndex < groupChains.size()
            && groupChains[(size_t) v.groupIndex] != nullptr)
            target = &groupBuffers[(size_t) v.groupIndex];

        // Per-group stereo balance (double-track "Stereo"): centre = unity both sides,
        // so a mono source panned hard-left keeps full level on L and silences R.
        float panL = 1.0f, panR = 1.0f;
        if (v.groupIndex >= 0 && v.groupIndex < groupPan.size())
        {
            const float pan = groupPan[v.groupIndex];
            if      (pan < 0.0f) panR = 1.0f + pan;   // toward left  → attenuate R
            else if (pan > 0.0f) panL = 1.0f - pan;   // toward right → attenuate L
        }

        for (int i = 0; i < numSamples; ++i)
        {
            if (! v.loopEnabled && v.position >= (double) frames)   // one-shot end
            {
                v.active = false;
                break;
            }

            // One-shot voices (ampEnv=false) play at full gain — the envelope is bypassed.
            const float env = v.ampEnv ? v.adsr.getNextSample() : 1.0f;
            const bool hasGroup = v.groupIndex >= 0 && v.groupIndex < groupVolume.size();
            const float gv  = hasGroup ? groupVolume[v.groupIndex]    : 1.0f;
            const float gtv = hasGroup ? groupTagVolume[v.groupIndex] : 1.0f;
            const float gg  = hasGroup ? groupGain[v.groupIndex]      : 1.0f;
            float trem = 1.0f;
            if (instTremActive)    trem *= instTrem[(size_t) (startSample + i)];
            if (groupTremActive)   trem *= groupTrem[(size_t) v.groupIndex][(size_t) (startSample + i)];
            const float g = env * v.gain * v.declickGain * gv * gtv * gg * trem * volDriftMul;

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int srcCh = juce::jmin (ch, srcChannels - 1);
                const float s = v.loopEnabled
                                  ? readLooped (v.buffer->audio, srcCh, v.position,
                                                v.loopEnd, v.loopLen, v.loopXf)
                                  : interpolate (v.buffer->audio, srcCh, v.position);
                const float panMul = (outChannels >= 2) ? (ch == 0 ? panL : (ch == 1 ? panR : 1.0f)) : 1.0f;
                target->addSample (ch, startSample + i, s * g * panMul);
            }

            v.position += v.rate * pitchBendMul * tuneMul;
            if (v.loopEnabled && v.position >= v.loopEnd)
                v.position -= v.loopLen;

            // Advance the anti-click ramp.
            v.declickGain += v.declickDelta;
            if (v.declickDelta > 0.0f && v.declickGain >= 1.0f)
            {
                v.declickGain = 1.0f;
                v.declickDelta = 0.0f;
            }

            if (v.fadingOut && v.declickGain <= 0.0f)   // choke/steal fade complete
            {
                v.active = false;
                break;
            }

            if (v.ampEnv && ! v.adsr.isActive())  // release finished (one-shots end at sample end instead)
            {
                v.active = false;
                break;
            }
        }
    }
}

void VoiceEngine::setLfoDepth (int position, float depth)
{
    if (position >= 0 && position < (int) mods.size())
        mods[(size_t) position].depth = depth;
}

void VoiceEngine::setLfoRate (int position, float hz)
{
    if (hz > 0.0f && position >= 0 && position < (int) mods.size())
        mods[(size_t) position].freqHz = hz;
}

void VoiceEngine::applyLfoBlock (int numSamples)
{
    if (numSamples <= 0 || mods.empty())
        return;

    // Reset this block's outputs. Only groups that actually have a tremolo modulator
    // (and the instrument buffer, if any) are cleared+read; the rest stay at unity.
    if (hasInstMod)
        for (int i = 0; i < numSamples; ++i) instTrem[(size_t) i] = 1.0f;
    for (int gi = 0; gi < groupHasTrem.size(); ++gi)
        if (groupHasTrem[gi])
        {
            auto& gt = groupTrem[(size_t) gi];
            for (int i = 0; i < numSamples; ++i) gt[(size_t) i] = 1.0f;
        }
    globalTuningMul = 1.0;
    for (auto& g : groupTuningModMul) g = 1.0;
    // Drift gate: for a library with a dedicated Drift button (hasDriftGateButton), the
    // wheels only take effect while that button's GLOBAL_TUNING modulator is engaged
    // (depth > 0). Libraries without such a button leave the gate open (wheel controls).
    driftGateOpen = ! hasDriftGateButton;

    // Tuning of the tremulant relative to the preset's binding ranges: the amplitude
    // (gain) swing on a per-group LEVEL effect is deepened a little, and pitch vibrato
    // is eased back a touch. (Matches the previous single-LFO behaviour.)
    constexpr double kGainSwing = 6.0;   // > 1 = more amplitude tremolo (LEVEL-target)
    constexpr double kTuneScale = 0.7;   // < 1 = less pitch vibrato (ranged tuning, e.g. tremulant)
    constexpr double kWowSemis  = 1.0;   // semitone span for a rangeless tuning wow (± modAmount*this)

    for (auto& m : mods)
    {
        if (m.freqHz <= 0.0)
            continue;
        const double inc = m.freqHz / sampleRate;

        // Per-sample amplitude tremolo (unipolar, downward: peak at unity, trough at
        // 1-depth). Multiple modulators on the same group multiply.
        if (m.depth > 0.0f && (m.ampInstrument || ! m.ampGroups.isEmpty()))
            for (int i = 0; i < numSamples; ++i)
            {
                const float w = lfoWave (m.shape, m.phase + inc * i);
                const float trem = 1.0f - m.depth * (0.5f - 0.5f * w);
                if (m.ampInstrument) instTrem[(size_t) i] *= trem;
                for (int gi : m.ampGroups) groupTrem[(size_t) gi][(size_t) i] *= trem;
            }

        // Control-rate targets (mid-block value). `waveMid` is the raw wave (-1..1);
        // `u` is its unipolar 0..depth form mapped through each binding's output range.
        const double waveMid = (double) lfoWave (m.shape, m.phase + inc * (numSamples * 0.5));
        const double u = (waveMid + 1.0) * 0.5 * (double) m.depth;
        for (const auto& b : m.bindings)
        {
            const double outMin = b.translationOutputMin.value_or (0.0);
            const double outMax = b.translationOutputMax.value_or (1.0);
            const double val = outMin + u * (outMax - outMin);

            if (b.parameter == "GLOBAL_TUNING" && hasDriftGateButton)
            {
                // This library's Drift button rides GLOBAL_TUNING: use its depth purely as
                // the on/off gate for the drift wheels (no global wow of its own).
                if (m.depth > 1.0e-4f)
                    driftGateOpen = true;
                continue;
            }

            if (b.parameter == "GLOBAL_TUNING" || (b.parameter == "GROUP_TUNING" && b.groupIndex))
            {
                // Pitch modulation (vibrato / wow). A binding with an explicit output
                // range uses it as-is — a small unipolar vibrato, e.g. the tremulant.
                // Without a range, `modAmount` alone is near-zero, so treat it as a
                // BIPOLAR wow depth (± modAmount·kWowSemis semitones) — an audible gentle
                // wander that centres on the true pitch.
                const bool hasRange = b.translationOutputMin.has_value() || b.translationOutputMax.has_value();
                const double semis = hasRange ? val * kTuneScale
                                              : waveMid * (double) m.depth * kWowSemis;
                const double mul = std::pow (2.0, semis / 12.0);
                if (b.parameter == "GLOBAL_TUNING")
                    globalTuningMul *= mul;
                else if (const int gi = *b.groupIndex; gi >= 0 && gi < groupTuningModMul.size())
                    groupTuningModMul.set (gi, groupTuningModMul[gi] * mul);
            }
            else if (b.groupIndex && b.effectIndex
                     && (b.parameter == "FX_FILTER_FREQUENCY" || b.parameter == "LEVEL" || b.parameter == "FX_MIX"))
            {
                const double v = (b.parameter == "LEVEL")
                                   ? outMin + u * (outMax - outMin) * kGainSwing   // deeper amplitude swing
                                   : val;
                setGroupEffectParam (*b.groupIndex, *b.effectIndex, b.parameter, (float) v);
            }
        }

        m.phase += inc * numSamples;
        m.phase -= std::floor (m.phase);
    }
}

void VoiceEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();

    // Advance + apply the LFO once for the whole block (amplitude tremolo, per-group
    // filter/gain sweep, pitch vibrato).
    applyLfoBlock (buffer.getNumSamples());

    // Per-group FX: voices render into per-group scratch buffers this block; clear
    // them to the block length first (no realloc — sized to maxBlock in prepare).
    const int numSamples = buffer.getNumSamples();
    if (anyGroupFx)
        for (size_t g = 0; g < groupBuffers.size(); ++g)
            if (groupChains[g] != nullptr)
            {
                groupBuffers[g].setSize (numChans, numSamples, false, false, true);
                groupBuffers[g].clear();
            }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
        renderChunk (buffer, pos, eventPos - pos);
        pos = eventPos;

        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
            handleNoteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            handleNoteOff (msg.getNoteNumber());
        else if (msg.isPitchWheel())
            handlePitchWheel (msg.getPitchWheelValue());
        else if (msg.isController() && msg.getControllerNumber() == 64)
            handleSustain (msg.getControllerValue());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            allNotesOff();
    }

    renderChunk (buffer, pos, numSamples - pos);

    // Run each group's FX chain over its scratch buffer, then sum into the output.
    if (anyGroupFx)
        for (size_t g = 0; g < groupBuffers.size(); ++g)
            if (groupChains[g] != nullptr)
            {
                groupChains[g]->process (groupBuffers[g]);
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.addFrom (ch, 0, groupBuffers[g],
                                    juce::jmin (ch, groupBuffers[g].getNumChannels() - 1), 0, numSamples);
            }
}

void VoiceEngine::allNotesOff()
{
    for (auto& v : voices)
        v.active = false;
}

int VoiceEngine::getActiveVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        if (v.active)
            ++n;
    return n;
}

CurvedAdsr::Parameters VoiceEngine::effectiveAdsr (const CurvedAdsr::Parameters& base, int groupIndex) const
{
    auto p = base;
    if (ovAttack  >= 0.0f) p.attack  = ovAttack;
    if (ovDecay   >= 0.0f) p.decay   = ovDecay;
    if (ovSustain >= 0.0f) p.sustain = ovSustain;
    if (ovRelease >= 0.0f) p.release = ovRelease;
    if (ovAttackCurve  > kNoCurve) p.attackCurve  = ovAttackCurve;
    if (ovDecayCurve   > kNoCurve) p.decayCurve   = ovDecayCurve;
    if (ovReleaseCurve > kNoCurve) p.releaseCurve = ovReleaseCurve;
    // Per-group overrides take precedence over the global ones.
    if (groupIndex >= 0 && groupIndex < groupAttack.size())
    {
        if (groupAttack [groupIndex] >= 0.0f) p.attack  = groupAttack [groupIndex];
        if (groupDecay  [groupIndex] >= 0.0f) p.decay   = groupDecay  [groupIndex];
        if (groupSustain[groupIndex] >= 0.0f) p.sustain = groupSustain[groupIndex];
        if (groupRelease[groupIndex] >= 0.0f) p.release = groupRelease[groupIndex];
    }
    return p;
}

// Amp overrides apply to NEW voices only — re-setting a sounding voice's ADSR
// mid-note can cut it off, so held notes keep the envelope they started with.
void VoiceEngine::setAmpAttack  (float s) { ovAttack  = s; }
void VoiceEngine::setAmpDecay   (float s) { ovDecay   = s; }
void VoiceEngine::setAmpSustain (float l) { ovSustain = l; }
void VoiceEngine::setAmpRelease (float s) { ovRelease = s; }

void VoiceEngine::setAmpAttackCurve  (float c) { ovAttackCurve  = c; }
void VoiceEngine::setAmpDecayCurve   (float c) { ovDecayCurve   = c; }
void VoiceEngine::setAmpReleaseCurve (float c) { ovReleaseCurve = c; }

void VoiceEngine::setGroupPan (int g, float pan)
{
    if (g >= 0 && g < groupPan.size())
        groupPan.set (g, juce::jlimit (-1.0f, 1.0f, pan));
}

void VoiceEngine::setGroupAmpAttack  (int g, float s) { if (g >= 0 && g < groupAttack.size())  groupAttack.set  (g, s); }
void VoiceEngine::setGroupAmpDecay   (int g, float s) { if (g >= 0 && g < groupDecay.size())   groupDecay.set   (g, s); }
void VoiceEngine::setGroupAmpSustain (int g, float l) { if (g >= 0 && g < groupSustain.size()) groupSustain.set (g, l); }
void VoiceEngine::setGroupAmpRelease (int g, float s) { if (g >= 0 && g < groupRelease.size()) groupRelease.set (g, s); }

void VoiceEngine::setGroupVolume (int groupIndex, float volume)
{
    if (groupIndex >= 0 && groupIndex < groupVolume.size())
        groupVolume.set (groupIndex, volume);
}

void VoiceEngine::setGroupTagVolume (int groupIndex, float volume)
{
    if (groupIndex >= 0 && groupIndex < groupTagVolume.size())
        groupTagVolume.set (groupIndex, volume);
}

void VoiceEngine::setGroupEnabled (int groupIndex, bool enabled)
{
    if (groupIndex >= 0 && groupIndex < groupEnabled.size())
        groupEnabled.set (groupIndex, enabled);
}

void VoiceEngine::setGroupTuning (int groupIndex, float semitones)
{
    if (groupIndex >= 0 && groupIndex < groupTuningMul.size())
        groupTuningMul.set (groupIndex, std::pow (2.0, semitones / 12.0));
}

void VoiceEngine::setGroupGain (int groupIndex, float db)
{
    if (groupIndex >= 0 && groupIndex < groupGain.size())
        groupGain.set (groupIndex, juce::Decibels::decibelsToGain (db));
}

void VoiceEngine::setGroupEffectParam (int groupIndex, int effectIndex, const juce::String& parameter, float value)
{
    if (groupIndex >= 0 && (size_t) groupIndex < groupChains.size() && groupChains[(size_t) groupIndex])
        groupChains[(size_t) groupIndex]->setEffectParam (effectIndex, parameter, value);
    else if (parameter == "LEVEL")
        setGroupGain (groupIndex, value);   // fallback: group with no chain (e.g. a single gain effect)
}

} // namespace dm
