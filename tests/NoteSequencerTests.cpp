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
