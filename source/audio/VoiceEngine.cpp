#include "VoiceEngine.h"
#include <cmath>

namespace dm
{

namespace
{
constexpr int kMaxVoices = 32;

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

    for (int gi = 0; gi < mode.groups.size(); ++gi)
    {
        const auto& g = mode.groups.getReference (gi);

        Zone proto;
        proto.adsr     = resolveAdsr (mode.amp, g);
        proto.gain     = (float) (g.volume   ? *g.volume   : mode.amp.volume);
        proto.velTrack = (float) (g.velTrack ? *g.velTrack : mode.amp.velTrack);
        proto.groupIndex = gi;
        proto.tags = g.tags;
        if (g.silencing)
            proto.silencedByTags = g.silencing->byTags;
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
            zones.add (z);
        }
    }
}

const VoiceEngine::Zone* VoiceEngine::findZone (int note) const
{
    for (const auto& z : zones)
        if (note >= z.loNote && note <= z.hiNote)
            return &z;
    return nullptr;
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
    const auto* zone = findZone (note);
    if (zone == nullptr)
        return;

    // Tag choke: stop any active voice that this voice's tags silence.
    // M2: a hard stop (mono retrigger). A short fade lands in a later milestone.
    if (! zone->tags.isEmpty())
    {
        for (auto& v : voices)
        {
            if (! v.active)
                continue;
            for (const auto& t : zone->tags)
                if (v.silencedByTags.contains (t))
                {
                    v.active = false;
                    break;
                }
        }
    }

    Voice* v = allocateVoice();
    v->active = true;
    v->buffer = zone->buffer;
    v->position = 0.0;
    v->note = note;
    v->groupIndex = zone->groupIndex;
    v->tags = zone->tags;
    v->silencedByTags = zone->silencedByTags;

    double rate = zone->buffer->sampleRate / sampleRate;
    if (zone->pitchKeyTrack)
        rate *= std::pow (2.0, (note - zone->rootNote) / 12.0);
    v->rate = rate;

    const float velGain = 1.0f - zone->velTrack + zone->velTrack * velocity;
    v->gain = zone->gain * velGain;

    v->adsr.setSampleRate (sampleRate);
    v->adsr.setParameters (zone->adsr);
    v->adsr.noteOn();
    v->startOrder = ++orderCounter;
}

void VoiceEngine::handleNoteOff (int note)
{
    for (auto& v : voices)
        if (v.active && v.note == note)
            v.adsr.noteOff();
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
            if (v.position >= (double) frames)   // one-shot reached the end
            {
                v.active = false;
                break;
            }

            const float env = v.adsr.getNextSample();

            for (int ch = 0; ch < outChannels; ++ch)
            {
                const int srcCh = juce::jmin (ch, srcChannels - 1);
                const float s = interpolate (v.buffer->audio, srcCh, v.position);
                buffer.addSample (ch, startSample + i, s * env * v.gain);
            }

            v.position += v.rate;

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

} // namespace dm
