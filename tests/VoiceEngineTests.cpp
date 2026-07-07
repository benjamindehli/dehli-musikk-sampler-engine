// Unit tests for the M2 voice engine.
//
// Synthesizes constant-DC PCM, encodes it to FLAC in memory, decodes it through
// EmbeddedFlacSource, then drives VoiceEngine via MIDI and asserts the rendered
// output: sample selection per note, amp ADSR on note-on/off, and monophonic
// tag-choke. Runs under the same console runner as ManifestLoaderTests.

#include <audio/VoiceEngine.h>
#include <audio/EmbeddedFlacSource.h>
#include <model/ManifestLoader.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace
{
constexpr double kSR = 48000.0;

juce::AudioBuffer<float> makeDc (float level, int frames, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, frames);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::fill (b.getWritePointer (ch), level, frames);
    return b;
}

// Two constant regions back to back: [0, n1) at level1, [n1, n1+n2) at level2.
juce::AudioBuffer<float> makeTwoRegions (float level1, int n1, float level2, int n2)
{
    juce::AudioBuffer<float> b (1, n1 + n2);
    auto* w = b.getWritePointer (0);
    juce::FloatVectorOperations::fill (w, level1, n1);
    juce::FloatVectorOperations::fill (w + n1, level2, n2);
    return b;
}

juce::MemoryBlock encodeFlac (const juce::AudioBuffer<float>& buf, double sr)
{
    juce::MemoryBlock mb;
    juce::FlacAudioFormat flac;

    auto* out = new juce::MemoryOutputStream (mb, false);
    std::unique_ptr<juce::AudioFormatWriter> writer (
        flac.createWriterFor (out, sr, (unsigned) buf.getNumChannels(), 16, {}, 0));

    if (writer == nullptr)
    {
        delete out;            // ownership not taken on failure
        return {};
    }

    writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
    writer.reset();            // flush + close (deletes the stream it owns)
    return mb;
}

float peakOfBlock (const juce::AudioBuffer<float>& b)
{
    return b.getMagnitude (0, b.getNumSamples());
}

class VoiceEngineTests : public juce::UnitTest
{
public:
    VoiceEngineTests() : juce::UnitTest ("VoiceEngine", "audio") {}

    void runTest() override
    {
        testSampleSelection();
        testAdsrRelease();
        testMonophonicChoke();
        testLooping();
        testInvalidLoopFallsBackToOneShot();
        testRoundRobinCyclic();
        testRoundRobinRandom();
        testGroupVolume();
        testAmpSustainOverride();
        testVelocityLayers();
        testMorphNote();
        testMorphNoteInRelease();
    }

    void testMorphNoteInRelease()
    {
        beginTest ("morphNote — a note already in amp release morphs and keeps its tail");

        // Same zones as testMorphNote but with a long (2 s) release so the tail is
        // clearly audible when the morph happens after note-off.
        auto a = encodeFlac (makeDc (0.30f, 48000), kSR);
        auto b = encodeFlac (makeTwoRegions (0.10f, 500, 0.90f, 47500), kSR);
        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:a", a.getData(), a.getSize()));
        expect (src.addFlac ("flac:b", b.getData(), b.getSize()));

        auto m = dm::loadManifestFromJson (R"({
            "schema":1,
            "modes":[{ "name":"MorphRel",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":2.0,"volume":1,"velTrack":0},
              "groups":[{ "samples":[
                { "source":"flac:a","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false },
                { "source":"flac:b","loNote":62,"hiNote":62,"rootNote":62,"sampleRate":48000,"pitchKeyTrack":false } ]}] }]
        })");
        expect (m.ok, "manifest should load");

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            ve.processBlock (out, midi);
        }

        // Release the note; the tail keeps sounding (2 s release).
        float before = 0.0f;
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            ve.processBlock (out, midi);
            before = out.getSample (0, 400);
            expect (before > 0.1f, "release tail still audible");
        }

        ve.morphNote (60, 62);

        // Position carried (≈ 3 blocks in → 0.90 region of the target): underlying
        // level triples (0.30 → 0.90) minus a little envelope decay. A restart from
        // the target's head would sit at 0.10 and come out BELOW `before`.
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            ve.processBlock (out, midi);
            const float after = out.getSample (0, 400);
            expect (after > before * 1.8f, "tail continues from the new sample at the carried position");
        }
    }

    void testMorphNote()
    {
        beginTest ("morphNote — crossfades to the new note's sample at the same elapsed time");

        // Note 60 → constant 0.30. Note 62 → 0.10 for the first 500 frames then 0.90:
        // if the morph carries the elapsed time (~512 frames) we land in the 0.90
        // region; a restart-from-zero would sit at 0.10 and fail.
        auto a = encodeFlac (makeDc (0.30f, 48000), kSR);
        auto b = encodeFlac (makeTwoRegions (0.10f, 500, 0.90f, 47500), kSR);
        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:a", a.getData(), a.getSize()));
        expect (src.addFlac ("flac:b", b.getData(), b.getSize()));

        auto m = dm::loadManifestFromJson (R"({
            "schema":1,
            "modes":[{ "name":"Morph",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":0,"volume":1,"velTrack":0},
              "groups":[{ "samples":[
                { "source":"flac:a","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false },
                { "source":"flac:b","loNote":62,"hiNote":62,"rootNote":62,"sampleRate":48000,"pitchKeyTrack":false } ]}] }]
        })");
        expect (m.ok, "manifest should load");

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        // Sound note 60 for one block.
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 400), 0.30f, 0.03f);
        }

        ve.morphNote (60, 62);

        // Next block: fade start still ≈ old level; past the ~4 ms crossfade the new
        // sample sounds — at the CARRIED position (0.90 region, not the 0.10 head).
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 2), 0.30f, 0.05f);
            expectWithinAbsoluteError (out.getSample (0, 400), 0.90f, 0.03f);
        }

        // The morphed voice now IS note 62: releasing 62 silences it (release = 0).
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOff (1, 62), 0);
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 400), 0.0f, 0.02f);
        }
    }

    void testVelocityLayers()
    {
        beginTest ("velocity layer selection");

        auto soft = encodeFlac (makeDc (0.30f, 1000), kSR);
        auto hard = encodeFlac (makeDc (0.90f, 1000), kSR);
        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:soft", soft.getData(), soft.getSize()));
        expect (src.addFlac ("flac:hard", hard.getData(), hard.getSize()));

        // Two groups on the same note, split by velocity (soft 0..63, hard 64..127).
        auto m = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [{
                "name": "Vel",
                "amp": { "attack":0.0,"decay":0.0,"sustain":1.0,"release":0.0,"volume":1.0,"velTrack":0.0 },
                "groups": [
                    { "velocity": {"lo":0,"hi":63},   "samples": [ { "source":"flac:soft","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000.0,"pitchKeyTrack":false } ] },
                    { "velocity": {"lo":64,"hi":127}, "samples": [ { "source":"flac:hard","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000.0,"pitchKeyTrack":false } ] }
                ]
            }]
        })");
        expect (m.ok, "manifest should load");

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        // Low velocity (40) → soft layer (0.30).
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 40), 0);
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 100), 0.30f, 0.03f);
        }

        // High velocity (100) → hard layer (0.90).
        ve.allNotesOff();
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 100), 0.90f, 0.03f);
        }
    }

    // One sustained DC sample on note 60, amp sustain 1 (so output == sample × gain).
    static juce::String oneSampleJson()
    {
        return R"({
            "schema":1,
            "modes":[{ "name":"One",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":0,"volume":1,"velTrack":0},
              "groups":[{ "samples":[
                { "source":"flac:a","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false } ]}] }]
        })";
    }

    void testGroupVolume()
    {
        beginTest ("group volume scales the output");
        auto flac = encodeFlac (makeDc (0.5f, 2000), kSR);
        dm::EmbeddedFlacSource src;
        src.addFlac ("flac:a", flac.getData(), flac.getSize());

        auto m = dm::loadManifestFromJson (oneSampleJson());
        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);
        ve.setGroupVolume (0, 0.5f);

        juce::AudioBuffer<float> out (1, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        ve.processBlock (out, midi);
        expectWithinAbsoluteError (out.getSample (0, 100), 0.25f, 0.02f);  // 0.5 × 0.5
    }

    void testAmpSustainOverride()
    {
        beginTest ("runtime amp sustain override scales a held note");
        auto flac = encodeFlac (makeDc (0.5f, 2000), kSR);
        dm::EmbeddedFlacSource src;
        src.addFlac ("flac:a", flac.getData(), flac.getSize());

        auto m = dm::loadManifestFromJson (oneSampleJson());
        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);
        ve.setAmpSustain (0.5f);   // override sustain 1 → 0.5

        juce::AudioBuffer<float> out (1, 512);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        ve.processBlock (out, midi);
        expectWithinAbsoluteError (out.getSample (0, 100), 0.25f, 0.02f);  // 0.5 × 0.5
    }

    // Builds a single round-robin group of 3 samples (levels 0.2/0.4/0.6) on note
    // 60, with the given seqMode, and returns the loaded result + source.
    static juce::String rrManifest (const juce::String& mode)
    {
        return juce::String (R"({
            "schema":1,"modes":[{"name":"RR",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":0,"volume":1,"velTrack":0},
              "groups":[{"roundRobin":{"mode":"MODE","length":3},"samples":[
                {"source":"flac:r1","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false,"seqPosition":1},
                {"source":"flac:r2","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false,"seqPosition":2},
                {"source":"flac:r3","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false,"seqPosition":3}]}]}]
        })").replace ("MODE", mode);
    }

    void addRrSamples (dm::EmbeddedFlacSource& src)
    {
        auto f1 = encodeFlac (makeDc (0.2f, 500), kSR);
        auto f2 = encodeFlac (makeDc (0.4f, 500), kSR);
        auto f3 = encodeFlac (makeDc (0.6f, 500), kSR);
        src.addFlac ("flac:r1", f1.getData(), f1.getSize());
        src.addFlac ("flac:r2", f2.getData(), f2.getSize());
        src.addFlac ("flac:r3", f3.getData(), f3.getSize());
    }

    float triggerLevel (dm::VoiceEngine& ve)
    {
        ve.allNotesOff();
        juce::AudioBuffer<float> out (1, 256);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        ve.processBlock (out, midi);
        return out.getSample (0, 100);
    }

    void testRoundRobinCyclic()
    {
        beginTest ("round-robin (round_robin) cycles through samples in order");

        dm::EmbeddedFlacSource src;
        addRrSamples (src);
        auto m = dm::loadManifestFromJson (rrManifest ("round_robin"));
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        const float expected[] = { 0.2f, 0.4f, 0.6f, 0.2f, 0.4f, 0.6f };
        for (int k = 0; k < 6; ++k)
            expectWithinAbsoluteError (triggerLevel (ve), expected[k], 0.02f);
    }

    void testRoundRobinRandom()
    {
        beginTest ("round-robin (random) picks varied, in-range samples");

        dm::EmbeddedFlacSource src;
        addRrSamples (src);
        auto m = dm::loadManifestFromJson (rrManifest ("random"));
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        bool hit[3] = { false, false, false };
        for (int k = 0; k < 40; ++k)
        {
            const float lvl = triggerLevel (ve);
            if      (std::abs (lvl - 0.2f) < 0.03f) hit[0] = true;
            else if (std::abs (lvl - 0.4f) < 0.03f) hit[1] = true;
            else if (std::abs (lvl - 0.6f) < 0.03f) hit[2] = true;
            else                                    expect (false, "unexpected level " + juce::String (lvl));
        }
        expect ((hit[0] + hit[1] + hit[2]) >= 2, "random should select more than one sample");
    }

    void testSampleSelection()
    {
        beginTest ("sample selection + native-rate playback");

        auto flacA = encodeFlac (makeDc (0.50f, 1000), kSR);
        auto flacB = encodeFlac (makeDc (0.25f, 1000), kSR);
        expect (flacA.getSize() > 0 && flacB.getSize() > 0, "FLAC encode failed");

        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:a", flacA.getData(), flacA.getSize()));
        expect (src.addFlac ("flac:b", flacB.getData(), flacB.getSize()));
        expectEquals (src.size(), 2);

        auto m = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [{
                "name": "Sel",
                "amp": { "attack": 0.0, "decay": 0.0, "sustain": 1.0, "release": 0.0, "volume": 1.0, "velTrack": 0.0 },
                "groups": [{
                    "samples": [
                        { "source": "flac:a", "loNote": 60, "hiNote": 60, "rootNote": 60, "sampleRate": 48000.0, "pitchKeyTrack": false },
                        { "source": "flac:b", "loNote": 72, "hiNote": 72, "rootNote": 72, "sampleRate": 48000.0, "pitchKeyTrack": false }
                    ]
                }]
            }]
        })");
        expect (m.ok, "manifest should load");

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        // Note 60 → sample A (0.50).
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            ve.processBlock (out, midi);
            expectEquals (ve.getActiveVoiceCount(), 1);
            expectWithinAbsoluteError (out.getSample (0, 100), 0.50f, 0.02f);
        }

        // Note 72 → sample B (0.25).
        ve.allNotesOff();
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 72, 1.0f), 0);
            ve.processBlock (out, midi);
            expectEquals (ve.getActiveVoiceCount(), 1);
            expectWithinAbsoluteError (out.getSample (0, 100), 0.25f, 0.02f);
        }

        // A note with no zone produces nothing.
        ve.allNotesOff();
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
            ve.processBlock (out, midi);
            expectEquals (ve.getActiveVoiceCount(), 0);
            expectWithinAbsoluteError (peakOfBlock (out), 0.0f, 1.0e-6f);
        }
    }

    void testAdsrRelease()
    {
        beginTest ("amp ADSR — sustain while held, silence after release");

        auto flac = encodeFlac (makeDc (0.5f, (int) kSR), kSR); // 1s of audio
        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:s", flac.getData(), flac.getSize()));

        auto m = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [{
                "name": "Env",
                "amp": { "attack": 0.0, "decay": 0.0, "sustain": 1.0, "release": 0.1, "volume": 1.0, "velTrack": 0.0 },
                "groups": [{ "samples": [
                    { "source": "flac:s", "loNote": 60, "hiNote": 60, "rootNote": 60, "sampleRate": 48000.0, "pitchKeyTrack": false }
                ] }]
            }]
        })");
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        // Hold: sustains at full level.
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            ve.processBlock (out, midi);
            expectWithinAbsoluteError (out.getSample (0, 256), 0.5f, 0.02f);
            expectEquals (ve.getActiveVoiceCount(), 1);
        }

        // Release (0.1s ≈ 4800 frames) then run well past it.
        {
            juce::AudioBuffer<float> out (1, 512);
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);
            ve.processBlock (out, midi); // first release block: still audible
            expect (peakOfBlock (out) > 0.05f, "release should start above silence");

            juce::MidiBuffer empty;
            for (int i = 0; i < 20; ++i) // ~10k frames, comfortably past release
                ve.processBlock (out, empty);

            expectEquals (ve.getActiveVoiceCount(), 0);
            expectWithinAbsoluteError (peakOfBlock (out), 0.0f, 1.0e-4f);
        }
    }

    void testMonophonicChoke()
    {
        beginTest ("monophonic tag-choke — new note silences previous");

        auto flacA = encodeFlac (makeDc (0.50f, (int) kSR), kSR);
        auto flacB = encodeFlac (makeDc (0.25f, (int) kSR), kSR);
        dm::EmbeddedFlacSource src;
        src.addFlac ("flac:a", flacA.getData(), flacA.getSize());
        src.addFlac ("flac:b", flacB.getData(), flacB.getSize());

        auto m = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [{
                "name": "Mono",
                "amp": { "attack": 0.0, "decay": 0.0, "sustain": 1.0, "release": 0.0, "volume": 1.0, "velTrack": 0.0 },
                "groups": [{
                    "tags": ["monophonic"],
                    "silencing": { "mode": "normal", "byTags": ["monophonic"] },
                    "samples": [
                        { "source": "flac:a", "loNote": 60, "hiNote": 60, "rootNote": 60, "sampleRate": 48000.0, "pitchKeyTrack": false },
                        { "source": "flac:b", "loNote": 62, "hiNote": 62, "rootNote": 62, "sampleRate": 48000.0, "pitchKeyTrack": false }
                    ]
                }]
            }]
        })");
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 512, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        juce::AudioBuffer<float> out (1, 512);
        {
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            ve.processBlock (out, midi);
            expectEquals (ve.getActiveVoiceCount(), 1);
            expectWithinAbsoluteError (out.getSample (0, 200), 0.50f, 0.02f);
        }
        {
            // Second mono note arrives without releasing the first.
            juce::MidiBuffer midi;
            midi.addEvent (juce::MidiMessage::noteOn (1, 62, 1.0f), 0);
            ve.processBlock (out, midi);
            expectEquals (ve.getActiveVoiceCount(), 1);                 // first was choked
            expectWithinAbsoluteError (out.getSample (0, 200), 0.25f, 0.02f);
        }
    }

    void testLooping()
    {
        beginTest ("loop region sustains past the sample end");

        // Intro 0.25 (frames 0..999), loop region 0.5 (frames 1000..1999).
        auto flac = encodeFlac (makeTwoRegions (0.25f, 1000, 0.5f, 1000), kSR);
        dm::EmbeddedFlacSource src;
        expect (src.addFlac ("flac:loop", flac.getData(), flac.getSize()));

        auto m = dm::loadManifestFromJson (R"({
            "schema":1,
            "modes":[{ "name":"Loop",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":0,"volume":1,"velTrack":0},
              "groups":[{ "samples":[
                { "source":"flac:loop","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false,
                  "loop":{"enabled":true,"start":1000,"end":2000,"crossfade":0} } ]}] }]
        })");
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 4096, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        juce::AudioBuffer<float> out (1, 3000);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        ve.processBlock (out, midi);

        expectWithinAbsoluteError (out.getSample (0, 500), 0.25f, 0.02f);  // intro
        expectWithinAbsoluteError (out.getSample (0, 1500), 0.5f, 0.02f);  // first pass of loop
        expectWithinAbsoluteError (out.getSample (0, 2500), 0.5f, 0.02f);  // wrapped → still looping
        expectEquals (ve.getActiveVoiceCount(), 1);                        // held: keeps looping
    }

    void testInvalidLoopFallsBackToOneShot()
    {
        beginTest ("loop points past the audio fall back to one-shot");

        auto flac = encodeFlac (makeTwoRegions (0.25f, 1000, 0.5f, 1000), kSR);
        dm::EmbeddedFlacSource src;
        src.addFlac ("flac:loop", flac.getData(), flac.getSize());

        auto m = dm::loadManifestFromJson (R"({
            "schema":1,
            "modes":[{ "name":"Loop",
              "amp":{"attack":0,"decay":0,"sustain":1,"release":0,"volume":1,"velTrack":0},
              "groups":[{ "samples":[
                { "source":"flac:loop","loNote":60,"hiNote":60,"rootNote":60,"sampleRate":48000,"pitchKeyTrack":false,
                  "loop":{"enabled":true,"start":1000,"end":99999,"crossfade":0} } ]}] }]
        })");
        expect (m.ok);

        dm::VoiceEngine ve;
        ve.prepare (kSR, 4096, 1);
        ve.setMode (m.library.modes.getReference (0), src);

        juce::AudioBuffer<float> out (1, 3000);
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
        ve.processBlock (out, midi);

        expectWithinAbsoluteError (out.getSample (0, 2500), 0.0f, 1.0e-4f); // ended at 2000
        expectEquals (ve.getActiveVoiceCount(), 0);
    }
};

VoiceEngineTests voiceEngineTests;
} // namespace
