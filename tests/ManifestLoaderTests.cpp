// Unit tests for the native JSON manifest loader (M1).
//
// Loads the hand-authored fixtures in tests/fixtures/ and asserts the parsed
// model, then exercises the loader's error handling with inline JSON. Built as a
// small console app (see CMakeLists.txt) and registered with CTest.

#include <model/ManifestLoader.h>
#include <juce_core/juce_core.h>
#include <iostream>
#include <map>

namespace
{
juce::File fixturesDir()
{
   #if defined (DMSE_TEST_FIXTURES_DIR)
    return juce::File (DMSE_TEST_FIXTURES_DIR);
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/fixtures");
   #endif
}

class ManifestLoaderTests : public juce::UnitTest
{
public:
    ManifestLoaderTests() : juce::UnitTest ("ManifestLoader", "manifest") {}

    dm::ManifestParseResult loadFixture (const juce::String& fixtureName)
    {
        auto file = fixturesDir().getChildFile (fixtureName);
        if (! file.existsAsFile())
        {
            expect (false, "fixture not found: " + file.getFullPathName());
            return {};
        }
        auto result = dm::loadManifestFromJson (file.loadFileAsString());
        for (auto& e : result.errors)
            logMessage ("  error: " + e);
        return result;
    }

    void runTest() override
    {
        testBass();
        testDrums();
        testWurli();
        testAutostrum();
        testSplitManifest();
        testErrors();
    }

    void testBass()
    {
        beginTest ("bass.json — mapping, ADSR, FX, full UI");
        auto r = loadFixture ("bass.json");
        expect (r.ok, "bass.json should load cleanly");
        expectEquals (r.library.schema, 1);
        expectEquals (r.library.library, juce::String ("Omni-84"));
        expectEquals (r.library.modes.size(), 1);

        const auto& mode = r.library.modes.getReference (0);
        expectEquals (mode.name, juce::String ("Bass"));
        expectWithinAbsoluteError (mode.amp.release, 0.1, 1.0e-9);
        expect (mode.amp.enabled);

        expectEquals (mode.tags.size(), 1);
        expectEquals (mode.tags.getReference (0).name, juce::String ("monophonic"));
        expect (mode.tags.getReference (0).polyphony.has_value());
        expectEquals (mode.tags.getReference (0).polyphony.value(), 1);

        expectEquals (mode.groups.size(), 1);
        const auto& group = mode.groups.getReference (0);
        expect (group.tags.contains ("monophonic"));
        expect (group.silencing.has_value());
        expectEquals (group.silencing->mode, juce::String ("normal"));
        expect (group.silencing->byTags.contains ("monophonic"));

        expectEquals (group.samples.size(), 3);
        const auto& s0 = group.samples.getReference (0);
        expectEquals (s0.source, juce::String ("flac:Bass_0C"));
        expectEquals (s0.rootNote, 24);
        expect (! s0.loop.enabled);
        expect (s0.lengthFrames.has_value() && s0.lengthFrames.value() == 805888);

        expectEquals (mode.effects.size(), 2);
        const auto& fx0 = mode.effects.getReference (0);
        expectEquals (fx0.type, juce::String ("lowpass"));
        expect (! fx0.enabled);
        expect (fx0.frequency.has_value());
        expectWithinAbsoluteError (fx0.frequency.value(), 15000.0, 1.0e-6);
        const auto& fx1 = mode.effects.getReference (1);
        expectEquals (fx1.type, juce::String ("convolution"));
        expectEquals (fx1.ir, juce::String ("ir:Space"));

        // UI
        expectEquals (mode.ui.width, 812);
        expectEquals (mode.ui.height, 375);
        expectEquals (mode.ui.tabs.size(), 1);
        const auto& tab = mode.ui.tabs.getReference (0);
        expectEquals (tab.controls.size(), 3);
        const auto& c0 = tab.controls.getReference (0);
        expectEquals (c0.label, juce::String ("Sustain"));
        expect (c0.skin.has_value());
        expect (c0.skin->numFrames.has_value() && c0.skin->numFrames.value() == 101);
        expectEquals (c0.bindings.size(), 1);
        expectEquals (c0.bindings.getReference (0).parameter, juce::String ("ENV_RELEASE"));

        const auto& cReverb = tab.controls.getReference (2);
        expect (cReverb.bindings.getReference (0).factor.has_value());
        expectWithinAbsoluteError (cReverb.bindings.getReference (0).factor.value(), 0.01, 1.0e-9);

        expectEquals (tab.buttons.size(), 1);
        const auto& btn = tab.buttons.getReference (0);
        expectEquals (btn.states.size(), 2);
        const auto& offState = btn.states.getReference (0);
        expectEquals (offState.name, juce::String ("Off"));
        expectEquals (offState.bindings.size(), 2);
        // fixed_value payload is polymorphic: bool here, asset-id string in the second.
        expect (offState.bindings.getReference (0).translationValue.isBool());
        expect (! (bool) offState.bindings.getReference (0).translationValue);
        expectEquals (offState.bindings.getReference (1).translationValue.toString(), juce::String ("img:light_off"));

        expectEquals (tab.images.size(), 1);
        expectEquals (mode.ui.keyboardColors.size(), 1);
        expectEquals (mode.ui.keyboardColors.getReference (0).color, juce::String ("FF222222"));
    }

    void testDrums()
    {
        beginTest ("drums.json — velocity, round-robin, triggers, gain, curves");
        auto r = loadFixture ("drums.json");
        expect (r.ok, "drums.json should load cleanly");
        const auto& mode = r.library.modes.getReference (0);
        expectEquals (mode.name, juce::String ("Kit"));

        expect (mode.amp.attackCurve.has_value());
        expectWithinAbsoluteError (mode.amp.attackCurve.value(), -100.0, 1.0e-9);

        expectEquals (mode.groups.size(), 2);
        const auto& g0 = mode.groups.getReference (0);
        expect (g0.roundRobin.has_value());
        expectEquals (g0.roundRobin->mode, juce::String ("random"));
        expect (g0.roundRobin->length.has_value() && g0.roundRobin->length.value() == 18);
        expect (g0.velocity.has_value());
        expectEquals (g0.velocity->hi, 127);
        expect (g0.decay.has_value());
        expectWithinAbsoluteError (g0.decay.value(), 0.15, 1.0e-9);
        expect (g0.samples.getReference (0).seqPosition.has_value());
        expectEquals (g0.samples.getReference (0).seqPosition.value(), 1);

        const auto& g1 = mode.groups.getReference (1);
        expectEquals (g1.trigger, juce::String ("release"));
        expect (g1.ampEnvEnabled.has_value() && g1.ampEnvEnabled.value() == false);
        expect (g1.samples.getReference (0).volume.has_value());

        // gain effect
        bool foundGain = false;
        for (const auto& fx : mode.effects)
            if (fx.type == "gain")
            {
                foundGain = true;
                expect (fx.gain.has_value());
                expectWithinAbsoluteError (fx.gain.value(), 10.0, 1.0e-9);
            }
        expect (foundGain, "expected a gain effect");

        const auto& fxLow = mode.effects.getReference (0);
        expect (fxLow.resonance.has_value());
    }

    void testWurli()
    {
        beginTest ("wurli.json — LFO modulator, velocity layers, loops");
        auto r = loadFixture ("wurli.json");
        expect (r.ok, "wurli.json should load cleanly");
        const auto& mode = r.library.modes.getReference (0);

        expectEquals (mode.modulators.size(), 1);
        const auto& lfo = mode.modulators.getReference (0);
        expectEquals (lfo.shape, juce::String ("sine"));
        expectWithinAbsoluteError (lfo.frequency, 5.69, 1.0e-9);
        expectEquals (lfo.bindings.size(), 2);
        const auto& mb = lfo.bindings.getReference (0);
        expectEquals (mb.modBehavior, juce::String ("set"));
        expect (mb.translationOutputMax.has_value());
        expectWithinAbsoluteError (mb.translationOutputMax.value(), 1.0, 1.0e-9);

        expectEquals (mode.groups.size(), 2);
        expect (mode.groups.getReference (0).velocity.has_value());
        expectEquals (mode.groups.getReference (0).velocity->hi, 63);
        expectEquals (mode.groups.getReference (1).velocity->lo, 64);

        const auto& s = mode.groups.getReference (0).samples.getReference (0);
        expect (s.loop.enabled);
        expect (s.loop.crossfade.has_value() && s.loop.crossfade.value() == 480);
    }

    void testAutostrum()
    {
        beginTest ("autostrum.json — note sequences + SEQ bindings");
        auto r = loadFixture ("autostrum.json");
        expect (r.ok, "autostrum.json should load cleanly");
        const auto& mode = r.library.modes.getReference (0);

        expectEquals (mode.sequences.size(), 2);
        const auto& seq = mode.sequences.getReference (0);
        expectEquals (seq.name, juce::String ("CMajor"));
        expect (seq.length.has_value() && seq.length.value() == 13);
        expectEquals (seq.notes.size(), 3);
        expectEquals (seq.notes.getReference (0).note, 48);

        const auto& tab = mode.ui.tabs.getReference (0);
        const auto& rate = tab.controls.getReference (0).bindings.getReference (0);
        expectEquals (rate.parameter, juce::String ("SEQ_PLAYBACK_RATE"));
        expect (rate.noteIndex.has_value() && rate.noteIndex.value() == 0);
        const auto& chord = tab.controls.getReference (1).bindings.getReference (0);
        expectEquals (chord.parameter, juce::String ("SEQ_INDEX"));
    }

    void testSplitManifest()
    {
        beginTest ("split manifest — $use / $ref merge + cycle detection");

        auto parse = [] (const char* json) { juce::var v; juce::JSON::parse (json, v); return v; };

        std::map<juce::String, juce::var> partials;
        // A shared convolution reverb, reused by both modes.
        partials["std-reverb"] = parse (R"({"type":"convolution","ir":"ir:hall","mix":0.5})");
        // A shared amp block pulled in via $use.
        partials["soft-amp"]   = parse (R"({"amp":{"attack":0.01,"release":0.4}})");

        std::map<juce::String, juce::var> modes;
        // Full: $use the amp block, and splice the reverb via $ref with a mix override.
        modes["full"] = parse (R"({
            "name":"Full", "$use":["soft-amp"],
            "groups":[{"samples":[{"source":"flac:a"}]}],
            "effects":[{"$ref":"std-reverb","mix":0.3}]
        })");
        // Lite: same reverb, unmodified.
        modes["lite"] = parse (R"({
            "name":"Lite",
            "groups":[{"samples":[{"source":"flac:a"}]}],
            "effects":[{"$ref":"std-reverb"}]
        })");

        auto index = parse (R"({"schema":1,"library":"Split","modes":["full","lite"]})");

        auto modeLoader    = [&] (const juce::String& n) { auto it = modes.find (n);    return it == modes.end()    ? juce::var() : it->second; };
        auto partialLoader = [&] (const juce::String& n) { auto it = partials.find (n); return it == partials.end() ? juce::var() : it->second; };

        juce::StringArray errors;
        auto merged = dm::resolveSplitManifest (index, modeLoader, partialLoader, errors);
        expect (errors.isEmpty(), "resolve should succeed: " + errors.joinIntoString ("; "));

        auto r = dm::loadManifest (merged);
        expect (r.ok, "merged split manifest should parse: " + r.errors.joinIntoString ("; "));
        expectEquals (r.library.library, juce::String ("Split"));
        expectEquals (r.library.modes.size(), 2);

        const auto& full = r.library.modes.getReference (0);
        expectEquals (full.name, juce::String ("Full"));
        expectWithinAbsoluteError (full.amp.attack, 0.01, 1.0e-9);   // from $use soft-amp
        expectWithinAbsoluteError (full.amp.release, 0.4, 1.0e-9);
        expectEquals (full.effects.size(), 1);
        expectEquals (full.effects.getReference (0).type, juce::String ("convolution"));  // from $ref
        expectEquals (full.effects.getReference (0).ir, juce::String ("ir:hall"));
        expect (full.effects.getReference (0).mix.has_value());
        expectWithinAbsoluteError (full.effects.getReference (0).mix.value(), 0.3, 1.0e-9); // override won

        const auto& lite = r.library.modes.getReference (1);
        expectWithinAbsoluteError (lite.effects.getReference (0).mix.value(), 0.5, 1.0e-9); // partial's own value

        // Unknown partial → error.
        {
            juce::StringArray e2;
            auto bad = parse (R"({"schema":1,"modes":[{"name":"M","effects":[{"$ref":"nope"}]}]})");
            dm::resolveSplitManifest (bad, modeLoader, partialLoader, e2);
            expect (! e2.isEmpty(), "unknown partial should report an error");
        }

        // Import cycle a → b → a → error (not infinite recursion).
        {
            std::map<juce::String, juce::var> cyc;
            cyc["a"] = parse (R"({"$use":["b"]})");
            cyc["b"] = parse (R"({"$use":["a"]})");
            auto cycLoader = [&] (const juce::String& n) { auto it = cyc.find (n); return it == cyc.end() ? juce::var() : it->second; };
            juce::StringArray e3;
            auto idx = parse (R"({"schema":1,"modes":[{"name":"M","$use":["a"],"groups":[{"samples":[{"source":"flac:x"}]}]}]})");
            dm::resolveSplitManifest (idx, modeLoader, cycLoader, e3);
            expect (! e3.isEmpty(), "import cycle should be reported");
        }
    }

    void testErrors()
    {
        beginTest ("loader error handling");

        auto bad = dm::loadManifestFromJson ("{ this is not json");
        expect (! bad.ok, "malformed JSON should fail");
        expect (! bad.errors.isEmpty());

        auto noModes = dm::loadManifestFromJson (R"({"schema":1})");
        expect (! noModes.ok, "missing modes array should fail");

        auto newer = dm::loadManifestFromJson (R"({"schema":99,"modes":[]})");
        expect (! newer.ok, "newer schema version should be rejected");

        auto missingSource = dm::loadManifestFromJson (
            R"({"schema":1,"modes":[{"name":"M","groups":[{"samples":[{"loNote":60}]}]}]})");
        expect (! missingSource.ok, "sample without source should fail");

        // A minimal but valid manifest still parses.
        auto minimal = dm::loadManifestFromJson (
            R"({"schema":1,"modes":[{"name":"M","groups":[{"samples":[{"source":"flac:x"}]}]}]})");
        expect (minimal.ok, "minimal valid manifest should load");
        expectEquals (minimal.library.modes.getReference (0).name, juce::String ("M"));
    }
};

ManifestLoaderTests manifestLoaderTests;

// Print test output to stdout so CI / ctest captures it.
class PrintingRunner : public juce::UnitTestRunner
{
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;
    }
};
} // namespace

int main()
{
    PrintingRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0, total = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        failures += result->failures;
        total    += result->passes + result->failures;
    }

    std::cout << "\n" << (failures == 0 ? "PASS" : "FAIL")
              << " — " << total << " checks, " << failures << " failure(s)" << std::endl;
    return failures == 0 ? 0 : 1;
}
