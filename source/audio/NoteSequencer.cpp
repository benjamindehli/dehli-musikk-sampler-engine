#include "NoteSequencer.h"
#include <algorithm>

namespace dm
{

void NoteSequencer::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    reset();
}

void NoteSequencer::reset()
{
    active.clear();
    streamPos = 0;
}

void NoteSequencer::configure (const Mode& mode)
{
    sequences = mode.sequences;
    triggers.clearQuick();
    triggerForNote.fill (-1);

    for (const auto& st : mode.sequenceTriggers)
    {
        Trigger t;
        t.sequence      = st.sequence;
        t.transpose     = st.transpose;
        t.rate          = st.rate;
        t.loop          = st.loop;
        t.trackVelocity = st.trackVelocity;
        t.swallow       = st.swallow;

        const int idx = triggers.size();
        triggers.add (t);
        if (st.note >= 0 && st.note < 128)
            triggerForNote[(size_t) st.note] = idx;   // last trigger wins per note
    }

    reset();
}

void NoteSequencer::setRate (double stepsPerSecond)
{
    rateOverride.store (stepsPerSecond);
}

void NoteSequencer::setIndexOffset (int offset)
{
    indexOffset.store (offset);
}

void NoteSequencer::startActive (const Trigger& t, int triggerNote, float velocity, juce::int64 startStream)
{
    if (sequences.isEmpty())
        return;

    Active a;
    a.seqIndex   = juce::jlimit (0, sequences.size() - 1, t.sequence + indexOffset.load());
    a.transpose  = t.transpose;
    a.velocity   = t.trackVelocity ? velocity : 1.0f;
    a.loop       = t.loop;
    a.triggerNote = triggerNote;
    a.startStream = startStream;

    double rate = rateOverride.load();
    if (rate <= 0.0) rate = t.rate;
    if (rate <= 0.0) rate = 10.0;
    a.samplesPerStep = sampleRate / rate;

    active.push_back (std::move (a));
}

void NoteSequencer::advanceActive (Active& a, juce::MidiBuffer& out, int numSamples)
{
    const auto& seq = sequences.getReference (a.seqIndex);
    const int count = seq.notes.size();

    auto emitOff = [&] (int note, int offset)
    {
        out.addEvent (juce::MidiMessage::noteOff (1, note), juce::jlimit (0, numSamples - 1, offset));
    };

    // Key released: stop sounding notes and finish.
    if (a.stopping)
    {
        const int off = (int) juce::jlimit<juce::int64> (0, numSamples - 1, a.stopStream - streamPos);
        for (auto& f : a.fired)
            if (! f.off) { emitOff (f.note, off); f.off = true; }
        a.done = true;
        return;
    }

    const double seqLenSteps = seq.length.has_value()
                                 ? (double) *seq.length
                                 : (count > 0 ? (double) seq.notes.getReference (count - 1).position + 1.0 : 0.0);

    // Fire notes whose start falls in this block.
    while (a.nextNote < count)
    {
        const auto& n = seq.notes.getReference (a.nextNote);
        const double fireStream = (double) a.startStream + (double) n.position * a.samplesPerStep;
        const double rel = fireStream - (double) streamPos;
        if (rel >= (double) numSamples)
            break;

        const int offset = (int) juce::jmax (0.0, rel);
        const int note = juce::jlimit (0, 127, n.note + a.transpose);
        const float vel = juce::jlimit (0.0f, 1.0f, (float) n.velocity * a.velocity);
        out.addEvent (juce::MidiMessage::noteOn (1, note, vel), juce::jmin (offset, numSamples - 1));

        a.fired.push_back ({ note, fireStream + juce::jmax (0.0, (double) n.length) * a.samplesPerStep, false });
        ++a.nextNote;
    }

    // Emit any note-offs that come due this block.
    for (auto& f : a.fired)
        if (! f.off)
        {
            const double rel = f.offStream - (double) streamPos;
            if (rel < (double) numSamples)
            {
                emitOff (f.note, (int) juce::jmax (0.0, rel));
                f.off = true;
            }
        }

    // End of cycle: loop or finish once everything has sounded + released.
    if (a.nextNote >= count)
    {
        bool allOff = true;
        for (const auto& f : a.fired)
            if (! f.off) { allOff = false; break; }

        if (a.loop && seqLenSteps > 0.0)
        {
            a.startStream += (juce::int64) (seqLenSteps * a.samplesPerStep);
            a.nextNote = 0;
        }
        else if (allOff)
        {
            a.done = true;
        }
    }

    // Drop fired notes whose off has been emitted (bounds memory).
    a.fired.erase (std::remove_if (a.fired.begin(), a.fired.end(),
                                   [] (const Fired& f) { return f.off; }),
                   a.fired.end());
}

void NoteSequencer::process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples)
{
    out.clear();

    for (const auto meta : in)
    {
        const int sp = juce::jlimit (0, juce::jmax (0, numSamples - 1), meta.samplePosition);
        const auto msg = meta.getMessage();

        if (msg.isNoteOn())
        {
            const int n = msg.getNoteNumber();
            const int ti = (n >= 0 && n < 128) ? triggerForNote[(size_t) n] : -1;
            if (ti >= 0)
            {
                startActive (triggers.getReference (ti), n, msg.getFloatVelocity(),
                             streamPos + sp);
                if (! triggers.getReference (ti).swallow)
                    out.addEvent (msg, sp);
            }
            else
            {
                out.addEvent (msg, sp);
            }
        }
        else if (msg.isNoteOff())
        {
            const int n = msg.getNoteNumber();
            const int ti = (n >= 0 && n < 128) ? triggerForNote[(size_t) n] : -1;
            if (ti >= 0)
            {
                for (auto& a : active)
                    if (! a.done && ! a.stopping && a.triggerNote == n)
                    {
                        a.stopping = true;
                        a.stopStream = streamPos + sp;
                    }
                if (! triggers.getReference (ti).swallow)
                    out.addEvent (msg, sp);
            }
            else
            {
                out.addEvent (msg, sp);
            }
        }
        else
        {
            out.addEvent (msg, sp);   // CC / pitch-bend / etc. pass through
        }
    }

    for (auto& a : active)
        advanceActive (a, out, numSamples);

    active.erase (std::remove_if (active.begin(), active.end(),
                                  [] (const Active& a) { return a.done; }),
                  active.end());

    streamPos += numSamples;
}

} // namespace dm
