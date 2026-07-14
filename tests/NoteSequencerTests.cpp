// Unit tests for the general NoteSequencer (M6).
//
// Drives the sequencer directly with input MIDI and asserts the produced MIDI:
// passthrough for non-trigger keys, and one-shot strums fired at the right sample
// offsets / pitches / velocities, plus transpose, swallow, and key-release stop.
// Free-running timing: with sampleRate 48000 and rate 480, one step = 100 samples.

#include <audio/NoteSequencer.h>
#include <model/ManifestLoader.h>
#include <juce_core/juce_core.h>
#include <vector>

namespace
{
constexpr double kSR = 48000.0;

dm::Mode loadMode (const juce::String& json)
{
    auto r = dm::loadManifestFromJson (json);
    return (r.ok && r.library.modes.size() > 0) ? r.library.modes.getReference (0) : dm::Mode{};
}

struct Ev { int sample; bool on; int note; float vel; };

std::vector<Ev> collect (const juce::MidiBuffer& b)
{
    std::vector<Ev> v;
    for (const auto meta : b)
    {
        const auto m = meta.getMessage();
        if (m.isNoteOn())       v.push_back ({ meta.samplePosition, true,  m.getNoteNumber(), m.getFloatVelocity() });
        else if (m.isNoteOff()) v.push_back ({ meta.samplePosition, false, m.getNoteNumber(), 0.0f });
    }
    return v;
}

int findOn  (const std::vector<Ev>& v, int note) { for (auto& e : v) if (e.on  && e.note == note) return e.sample; return -1; }
int findOff (const std::vector<Ev>& v, int note) { for (auto& e : v) if (! e.on && e.note == note) return e.sample; return -1; }
bool hasNote (const std::vector<Ev>& v, int note) { for (auto& e : v) if (e.note == note) return true; return false; }

// A 3-note triad sequence triggered by key 36, with the given trigger attributes.
juce::String strumJson (int transpose, bool swallow)
{
    return juce::String (R"({
        "schema":1,
        "modes":[{ "name":"Strum", "groups":[{}],
          "sequences":[{ "name":"Triad","length":3,"rate":1,"notes":[
            {"position":0,"note":60,"velocity":1,"length":1},
            {"position":1,"note":64,"velocity":1,"length":1},
            {"position":2,"note":67,"velocity":1,"length":1}]}],
          "sequenceTriggers":[
            {"note":36,"sequence":0,"transpose":TRANSPOSE,"rate":480,"loop":false,"trackVelocity":true,"swallow":SWALLOW}]
        }]
    })").replace ("TRANSPOSE", juce::String (transpose))
        .replace ("SWALLOW", swallow ? "true" : "false");
}

class NoteSequencerTests : public juce::UnitTest
{
public:
    NoteSequencerTests() : juce::UnitTest ("NoteSequencer", "audio") {}

    void runTest() override
    {
        testPassthrough();
        testOneShotStrum();
        testTranspose();
        testNoSwallowPassesKey();
        testVelocityTracking();
        testKeyReleaseStops();
        testIndexOffset();
        testSelectStrumFires();
        testSelectStrumOrderAndRate();
        testChordChangeMorphs();
        testStrumKeyReleaseStops();
        testReleasedTailMorphs();
        testTempoSync();
    }

    void testTempoSync()
    {
        beginTest ("tempo sync: steps are 16th notes at the set BPM, overriding trigger rate");
        auto mode = loadMode (strumJson (0, true));   // trigger rate 480 — overridden below
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // 600 BPM, 16th steps → 40 steps/s → 1200 samples per step at 48 kHz.
        s.setTempoSync (true);
        s.setBpm (600.0);
        s.setBeatsPerStep (0.25);

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in, out, 4096);
        auto ev = collect (out);
        expectEquals (findOn (ev, 60), 0);
        expectEquals (findOn (ev, 64), 1200);
        expectEquals (findOn (ev, 67), 2400);

        // 8th-note steps at the same tempo → half the rate → 2400-sample steps.
        s.setBeatsPerStep (0.5);
        juce::MidiBuffer in8, out8;
        in8.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in8, out8, 8192);
        auto e8 = collect (out8);
        const int b8 = findOn (e8, 60);
        expect (b8 >= 0 && findOn (e8, 64) == b8 + 2400, "8th steps double the step length");

        // With a StrumSpeed knob present its NORMALISED position scrolls the
        // note-value table evenly slowest -> fastest, independent of BPM (the raw
        // steps/s override is ignored while synced). Full-up = 1/32 triplet: at
        // 600 BPM that is 120 steps/s -> 400-sample steps.
        s.setRate (480.0);   // raw override must NOT shift the synced mapping
        s.setRateNorm (0.999);
        juce::MidiBuffer inO, outO;
        inO.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (inO, outO, 8192);
        auto eO = collect (outO);
        const int bO = findOn (eO, 60);
        expect (bO >= 0 && findOn (eO, 64) == bO + 400, "knob full up = fastest note value");

        // Full-down = 1/4 dotted (slowest): 600 BPM -> 6.67 steps/s -> 7200-sample steps.
        s.setRateNorm (0.0);
        juce::MidiBuffer inD, outD;
        inD.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (inD, outD, 16384);
        auto eD = collect (outD);
        const int bD = findOn (eD, 60);
        expect (bD >= 0 && findOn (eD, 64) == bD + 7200, "knob full down = slowest note value");
        s.setRate (0.0);

        // Back to free: per-trigger rate (480 steps/s → 100-sample steps) again.
        s.setTempoSync (false);
        juce::MidiBuffer in2, out2;
        in2.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in2, out2, 4096);
        auto e2 = collect (out2);
        const int base = findOn (e2, 60);
        expect (base >= 0 && findOn (e2, 64) == base + 100, "free mode restores the trigger rate");
    }

    void testReleasedTailMorphs()
    {
        beginTest ("select+strum: chord change morphs notes already released (amp-release tail)");
        auto mode = selectStrumMode();
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // Strum chord A fully, then release the strum key: note-offs are emitted,
        // but the notes may still ring in the voices' amp release.
        juce::MidiBuffer in1, out1;
        in1.addEvent (juce::MidiMessage::noteOn (1, 24, 1.0f), 0);
        s.process (in1, out1, 512);

        juce::MidiBuffer in2, out2;
        in2.addEvent (juce::MidiMessage::noteOff (1, 24), 0);
        s.process (in2, out2, 512);
        auto e2 = collect (out2);
        expectEquals (findOff (e2, 60), 0);   // all released

        // Chord change AFTER release: the tails must still morph to chord B.
        juce::MidiBuffer in3, out3;
        juce::Array<dm::NoteSequencer::NoteMorph> morphs;
        in3.addEvent (juce::MidiMessage::noteOn (1, 38, 1.0f), 0);
        s.process (in3, out3, 512, &morphs);

        expectEquals (morphs.size(), 3);
        bool m60 = false, m64 = false, m67 = false;
        for (const auto& m : morphs)
        {
            if (m.from == 60 && m.to == 62) m60 = true;
            if (m.from == 64 && m.to == 65) m64 = true;
            if (m.from == 67 && m.to == 69) m67 = true;
        }
        expect (m60 && m64 && m67, "all three released notes morph to the new chord");
        expect (collect (out3).empty(), "no retriggers, no passthrough");
    }

    // Omnichord select+strum fixture: two chords (A = trigger 36 → seq 0, B = trigger 38
    // → seq 2), each with an "up" and a "down" ordering at consecutive seq indices, and
    // two strum keys (24 = offset 0/up, 26 = offset 1/down, with its own rate).
    // Long note lengths keep everything ringing so chord changes have notes to morph.
    dm::Mode selectStrumMode()
    {
        return loadMode (R"({
            "schema":1,
            "modes":[{ "name":"Omni","groups":[{}],
              "sequences":[
                {"name":"A up","length":1000,"rate":1,"notes":[
                  {"position":0,"note":60,"velocity":1,"length":1000},
                  {"position":1,"note":64,"velocity":1,"length":1000},
                  {"position":2,"note":67,"velocity":1,"length":1000}]},
                {"name":"A down","length":1000,"rate":1,"notes":[
                  {"position":0,"note":67,"velocity":1,"length":1000},
                  {"position":1,"note":64,"velocity":1,"length":1000},
                  {"position":2,"note":60,"velocity":1,"length":1000}]},
                {"name":"B up","length":1000,"rate":1,"notes":[
                  {"position":0,"note":62,"velocity":1,"length":1000},
                  {"position":1,"note":65,"velocity":1,"length":1000},
                  {"position":2,"note":69,"velocity":1,"length":1000}]},
                {"name":"B down","length":1000,"rate":1,"notes":[
                  {"position":0,"note":69,"velocity":1,"length":1000},
                  {"position":1,"note":65,"velocity":1,"length":1000},
                  {"position":2,"note":62,"velocity":1,"length":1000}]}],
              "sequenceTriggers":[
                {"note":36,"sequence":0,"transpose":0,"rate":480,"loop":false,"trackVelocity":true,"swallow":true},
                {"note":38,"sequence":2,"transpose":0,"rate":480,"loop":false,"trackVelocity":true,"swallow":true}],
              "strumKeys":[
                {"note":24,"seqOffset":0},
                {"note":26,"seqOffset":1,"rate":960}]
            }]
        })");
    }

    void testSelectStrumFires()
    {
        beginTest ("select+strum: chord keys only select, strum keys fire");
        auto mode = selectStrumMode();
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // Chord key alone fires nothing (and is swallowed).
        {
            juce::MidiBuffer in, out;
            in.addEvent (juce::MidiMessage::noteOn (1, 38, 1.0f), 0);
            s.process (in, out, 512);
            expect (collect (out).empty(), "chord key must not trigger or pass through");
        }
        // Strum key fires the SELECTED chord (B, from the key above).
        {
            juce::MidiBuffer in, out;
            in.addEvent (juce::MidiMessage::noteOn (1, 24, 1.0f), 0);
            s.process (in, out, 512);
            auto ev = collect (out);
            expect (! hasNote (ev, 24), "strum key is always swallowed");
            expectEquals (findOn (ev, 62), 0);
            expectEquals (findOn (ev, 65), 100);
            expectEquals (findOn (ev, 69), 200);
        }
    }

    void testSelectStrumOrderAndRate()
    {
        beginTest ("select+strum: strum key's seqOffset picks the ordering, rate its speed");
        auto mode = selectStrumMode();
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // No chord key pressed yet → trigger 0 (chord A) is pre-selected.
        // Key 26 = offset 1 (A down) at its own rate 960 → step = 50 samples.
        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 26, 1.0f), 0);
        s.process (in, out, 512);
        auto ev = collect (out);
        expectEquals (findOn (ev, 67), 0);
        expectEquals (findOn (ev, 64), 50);
        expectEquals (findOn (ev, 60), 100);
    }

    void testChordChangeMorphs()
    {
        beginTest ("select+strum: chord change morphs ringing notes, retargets unfired ones");
        auto mode = selectStrumMode();
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // Strum chord A (default selection). 150-sample block → 60 and 64 have fired,
        // 67 (position 2 = sample 200) has not.
        juce::MidiBuffer in1, out1;
        in1.addEvent (juce::MidiMessage::noteOn (1, 24, 1.0f), 0);
        s.process (in1, out1, 150);
        auto e1 = collect (out1);
        expect (hasNote (e1, 60) && hasNote (e1, 64) && ! hasNote (e1, 67));

        // Change to chord B while A rings: ringing notes morph 60→62, 64→65 (no
        // retrigger events), and the pending third note fires from B (69, not 67).
        juce::MidiBuffer in2, out2;
        juce::Array<dm::NoteSequencer::NoteMorph> morphs;
        in2.addEvent (juce::MidiMessage::noteOn (1, 38, 1.0f), 0);
        s.process (in2, out2, 512, &morphs);

        expectEquals (morphs.size(), 2);
        bool m60 = false, m64 = false;
        for (const auto& m : morphs)
        {
            if (m.from == 60 && m.to == 62) m60 = true;
            if (m.from == 64 && m.to == 65) m64 = true;
        }
        expect (m60 && m64, "both ringing notes morph to the new chord");

        auto e2 = collect (out2);
        expect (! hasNote (e2, 62) && ! hasNote (e2, 65), "morphed notes are not retriggered");
        expectEquals (findOn (e2, 69), 50);   // third step (stream 200) lands 50 into this block
        expect (! hasNote (e2, 67), "unfired note comes from the new chord");
    }

    void testStrumKeyReleaseStops()
    {
        beginTest ("select+strum: strum key release stops its notes (after a morph too)");
        auto mode = selectStrumMode();
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        juce::MidiBuffer in1, out1;
        in1.addEvent (juce::MidiMessage::noteOn (1, 24, 1.0f), 0);
        s.process (in1, out1, 150);

        // Chord change morphs 60→62, 64→65; then releasing the strum key must
        // release the MORPHED pitches.
        juce::MidiBuffer in2, out2;
        in2.addEvent (juce::MidiMessage::noteOn (1, 38, 1.0f), 0);
        s.process (in2, out2, 100, nullptr);

        juce::MidiBuffer in3, out3;
        in3.addEvent (juce::MidiMessage::noteOff (1, 24), 0);
        s.process (in3, out3, 512);
        auto e3 = collect (out3);
        expectEquals (findOff (e3, 62), 0);
        expectEquals (findOff (e3, 65), 0);
        expect (! hasNote (e3, 24), "strum key note-off is swallowed");
    }

    void testPassthrough()
    {
        beginTest ("non-trigger keys pass straight through");
        auto mode = loadMode (R"({"schema":1,"modes":[{"name":"P","groups":[{}]}]})");
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);
        expect (! s.hasTriggers());

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 10);
        in.addEvent (juce::MidiMessage::noteOff (1, 60), 200);
        s.process (in, out, 512);

        auto ev = collect (out);
        expectEquals ((int) ev.size(), 2);
        expectEquals (findOn (ev, 60), 10);
        expectEquals (findOff (ev, 60), 200);
    }

    void testOneShotStrum()
    {
        beginTest ("one-shot strum fires notes at the right offsets");
        auto mode = loadMode (strumJson (0, true));
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);
        expect (s.hasTriggers());

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in, out, 512);

        auto ev = collect (out);
        expect (! hasNote (ev, 36), "trigger key should be swallowed");
        expectEquals (findOn (ev, 60), 0);
        expectEquals (findOn (ev, 64), 100);
        expectEquals (findOn (ev, 67), 200);
        expectEquals (findOff (ev, 60), 100);
        expectEquals (findOff (ev, 64), 200);
        expectEquals (findOff (ev, 67), 300);
    }

    void testTranspose()
    {
        beginTest ("transpose shifts the fired notes");
        auto mode = loadMode (strumJson (12, true));
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in, out, 512);

        auto ev = collect (out);
        expectEquals (findOn (ev, 72), 0);
        expectEquals (findOn (ev, 76), 100);
        expectEquals (findOn (ev, 79), 200);
    }

    void testNoSwallowPassesKey()
    {
        beginTest ("swallow=false also passes the trigger key through");
        auto mode = loadMode (strumJson (0, false));
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in, out, 512);

        auto ev = collect (out);
        expect (hasNote (ev, 36), "trigger key should pass through when not swallowed");
        expect (findOn (ev, 60) >= 0, "strum still fires");
    }

    void testVelocityTracking()
    {
        beginTest ("trackVelocity scales fired velocity by the key velocity");
        auto mode = loadMode (strumJson (0, true));
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        juce::MidiBuffer in, out;
        in.addEvent (juce::MidiMessage::noteOn (1, 36, 0.5f), 0);
        s.process (in, out, 512);

        auto ev = collect (out);
        float firedVel = -1.0f;
        for (auto& e : ev) if (e.on && e.note == 60) firedVel = e.vel;
        expect (firedVel >= 0.0f, "note 60 should fire");
        expectWithinAbsoluteError (firedVel, 0.5f, 0.02f);
    }

    void testKeyReleaseStops()
    {
        beginTest ("releasing the trigger key stops a still-sounding note");

        // One long note so it's still sounding when the key is released next block.
        auto mode = loadMode (R"({
            "schema":1,
            "modes":[{ "name":"Hold","groups":[{}],
              "sequences":[{ "name":"Long","length":1000,"rate":1,"notes":[
                {"position":0,"note":60,"velocity":1,"length":1000}]}],
              "sequenceTriggers":[
                {"note":36,"sequence":0,"transpose":0,"rate":480,"loop":false,"trackVelocity":true,"swallow":true}]
            }]
        })");
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        juce::MidiBuffer in1, out1;
        in1.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
        s.process (in1, out1, 512);
        auto e1 = collect (out1);
        expectEquals (findOn (e1, 60), 0);
        expectEquals (findOff (e1, 60), -1);          // not released yet (off ≈ sample 100000)

        juce::MidiBuffer in2, out2;
        in2.addEvent (juce::MidiMessage::noteOff (1, 36), 0);
        s.process (in2, out2, 512);
        auto e2 = collect (out2);
        expectEquals (findOff (e2, 60), 0);           // key release stops the sounding note
    }

    void testIndexOffset()
    {
        beginTest ("index offset selects a different sequence (SEQ_INDEX / chord menu)");

        // Two sequences (note 60 vs 72); one trigger references sequence 0.
        auto mode = loadMode (R"({
            "schema":1,
            "modes":[{ "name":"Off","groups":[{}],
              "sequences":[
                {"name":"A","length":1,"rate":1,"notes":[{"position":0,"note":60,"velocity":1,"length":1}]},
                {"name":"B","length":1,"rate":1,"notes":[{"position":0,"note":72,"velocity":1,"length":1}]}],
              "sequenceTriggers":[
                {"note":36,"sequence":0,"transpose":0,"rate":480,"loop":false,"trackVelocity":true,"swallow":true}]
            }]
        })");
        dm::NoteSequencer s;
        s.prepare (kSR);
        s.configure (mode);

        // offset 0 → sequence 0 (note 60)
        {
            juce::MidiBuffer in, out;
            in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
            s.process (in, out, 512);
            auto ev = collect (out);
            expect (hasNote (ev, 60) && ! hasNote (ev, 72), "offset 0 → sequence A");
        }
        // offset 1 → sequence 1 (note 72)
        {
            s.setIndexOffset (1);
            juce::MidiBuffer in, out;
            in.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
            s.process (in, out, 512);
            auto ev = collect (out);
            expect (hasNote (ev, 72) && ! hasNote (ev, 60), "offset 1 → sequence B");
        }
    }
};

NoteSequencerTests noteSequencerTests;
} // namespace
