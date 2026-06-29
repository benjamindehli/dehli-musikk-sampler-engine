#include "VoiceEngine.h"
#include <cmath>

namespace dm
{

namespace
{
constexpr int kMaxVoices = 64;   // headroom for multi-group layering (drums stack several layers per hit)

// Resolve the effective amp envelope for a group: mode defaults, with the group's
// own overrides applied where present.
juce::ADSR::Parameters resolveAdsr (const AmpEnvelope& amp, const Group& g)
{
    juce::ADSR::Parameters p;
    p.attack  = (float) amp.attack;
    p.decay   = (float) (g.decay   ? *g.decay   : amp.decay);
    p.sustain = (float) amp.sustain;
    p.release = (float) (g.release ? *g.release : amp.release);
    return p;
}

float interpolate (const juce::AudioBuffer<float>& buf, int ch, double pos)
{
    const int i0 = (int) pos;
    const int n  = buf.getNumSamples();
    if (i0 < 0 || i0 >= n)
        return 0.0f;

    const float s0 = buf.getSample (ch, i0);
    const int i1 = i0 + 1;
    if (i1 >= n)
        return s0;

    const float frac = (float) (pos - (double) i0);
    return s0 + frac * (buf.getSample (ch, i1) - s0);
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

void VoiceEngine::prepare (double newSampleRate, int /*maxBlockSize*/, int /*numChannels*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    for (auto& v : voices)
    {
        v.active = false;
        v.adsr.setSampleRate (sampleRate);
    }
}

void VoiceEngine::releaseResources()
{
    allNotesOff();
}

void VoiceEngine::setMode (const Mode& mode, const SampleSource& source)
{
    allNotesOff();
    zones.clearQuick();

    const int numGroups = mode.groups.size();
    rrCounter.clearQuick(); rrCounter.insertMultiple (0, 0, numGroups);
    rrLast.clearQuick();    rrLast.insertMultiple (0, -1, numGroups);
    rrRandom.setSeed (20240101);   // fixed → reproducible variation across sessions
    groupVolume.clearQuick();    groupVolume.insertMultiple (0, 1.0f, numGroups);
    groupEnabled.clearQuick();   groupEnabled.insertMultiple (0, true, numGroups);
    groupTuningMul.clearQuick(); groupTuningMul.insertMultiple (0, 1.0, numGroups);

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

            Zone z = proto;
            z.loNote = s.loNote;
            z.hiNote = s.hiNote;
            z.rootNote = s.rootNote;
            z.buffer = buffer;
            z.pitchKeyTrack = s.pitchKeyTrack || groupPitchKeyTrack;

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

    // Start a voice for EVERY enabled group covering this note+velocity — drums
    // layer several groups on one note (and velocity layers are separate groups);
    // round-robin picks within each group.
    const int numGroups = groupEnabled.size();
    for (int g = 0; g < numGroups; ++g)
    {
        if (! groupEnabled[g])
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
    v->groupIndex = zone.groupIndex;
    v->tags = zone.tags;
    v->silencedByTags = zone.silencedByTags;

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

    v->baseAdsr = zone.adsr;
    v->adsr.setSampleRate (sampleRate);
    v->adsr.setParameters (effectiveAdsr (v->baseAdsr));
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
    for (auto& v : voices)
        if (v.active && v.note == note)
            v.adsr.noteOff();
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

    for (auto& v : voices)
    {
        if (! v.active)
            continue;

        const int srcChannels = v.buffer->getNumChannels();
        const int frames = v.buffer->getNumFrames();

        for (int i = 0; i < numSamples; ++i)
        {
            if (! v.loopEnabled && v.position >= (double) frames)   // one-shot end
            {
                v.active = false;
                break;
            }

            const float env = v.adsr.getNextSample();
            const float gv = (v.groupIndex >= 0 && v.groupIndex < groupVolume.size())
                                 ? groupVolume[v.groupIndex] : 1.0f;
            const float g = env * v.gain * v.declickGain * gv;

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int srcCh = juce::jmin (ch, srcChannels - 1);
                const float s = v.loopEnabled
                                  ? readLooped (v.buffer->audio, srcCh, v.position,
                                                v.loopEnd, v.loopLen, v.loopXf)
                                  : interpolate (v.buffer->audio, srcCh, v.position);
                buffer.addSample (ch, startSample + i, s * g);
            }

            v.position += v.rate * pitchBendMul;
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

            if (! v.adsr.isActive())             // release finished
            {
                v.active = false;
                break;
            }
        }
    }
}

void VoiceEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();

    int pos = 0;
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, buffer.getNumSamples(), meta.samplePosition);
        renderChunk (buffer, pos, eventPos - pos);
        pos = eventPos;

        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
            handleNoteOn (msg.getNoteNumber(), msg.getFloatVelocity());
        else if (msg.isNoteOff())
            handleNoteOff (msg.getNoteNumber());
        else if (msg.isPitchWheel())
            handlePitchWheel (msg.getPitchWheelValue());
        else if (msg.isAllNotesOff() || msg.isAllSoundOff())
            allNotesOff();
    }

    renderChunk (buffer, pos, buffer.getNumSamples() - pos);
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

juce::ADSR::Parameters VoiceEngine::effectiveAdsr (const juce::ADSR::Parameters& base) const
{
    auto p = base;
    if (ovAttack  >= 0.0f) p.attack  = ovAttack;
    if (ovDecay   >= 0.0f) p.decay   = ovDecay;
    if (ovSustain >= 0.0f) p.sustain = ovSustain;
    if (ovRelease >= 0.0f) p.release = ovRelease;
    return p;
}

// Amp overrides apply to NEW voices only — re-setting a sounding voice's ADSR
// mid-note can cut it off, so held notes keep the envelope they started with.
void VoiceEngine::setAmpAttack  (float s) { ovAttack  = s; }
void VoiceEngine::setAmpDecay   (float s) { ovDecay   = s; }
void VoiceEngine::setAmpSustain (float l) { ovSustain = l; }
void VoiceEngine::setAmpRelease (float s) { ovRelease = s; }

void VoiceEngine::setGroupVolume (int groupIndex, float volume)
{
    if (groupIndex >= 0 && groupIndex < groupVolume.size())
        groupVolume.set (groupIndex, volume);
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

} // namespace dm
