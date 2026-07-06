// Hand-authoring safety tests (Tier 3E).
//
// 1. kitchensink.json exercises EVERY schema field. It must load with ZERO
//    warnings (lint-clean + validation-clean), and the risky fields must survive
//    a write→reload — byte-idempotence alone can't catch "loader reads it but the
//    writer drops it" (both outputs simply lack the key).
// 2. Unknown keys (typos) must produce warnings instead of vanishing silently.
// 3. Dangling references (targetId, sequence index, button links) must warn.

#include <model/ManifestLoader.h>
#include <model/ManifestWriter.h>
#include <juce_core/juce_core.h>

namespace
{
juce::File lintFixturesDir()
{
   #if defined (DMSE_TEST_FIXTURES_DIR)
    return juce::File (DMSE_TEST_FIXTURES_DIR);
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/fixtures");
   #endif
}

class ManifestLintTests : public juce::UnitTest
{
public:
    ManifestLintTests() : juce::UnitTest ("ManifestLint", "manifest") {}

    void runTest() override
    {
        kitchenSink();
        unknownKeys();
        danglingReferences();
    }

    void kitchenSink()
    {
        beginTest ("kitchensink.json — every field loads clean and survives write->reload");

        auto file = lintFixturesDir().getChildFile ("kitchensink.json");
        expect (file.existsAsFile(), "fixture not found: " + file.getFullPathName());

        auto m1 = dm::loadManifestFromJson (file.loadFileAsString());
        expect (m1.ok, "kitchensink should load: " + m1.errors.joinIntoString ("; "));
        expect (m1.warnings.isEmpty(),
                "kitchensink must be lint/validation clean, got: " + m1.warnings.joinIntoString (" | "));

        // Write → reload; the reload must ALSO be warning-free (writer emits only
        // known keys) and the risky fields must still be there.
        auto m2 = dm::loadManifestFromJson (dm::writeManifestToJson (m1.library));
        expect (m2.ok, "writer output should reload");
        expect (m2.warnings.isEmpty(),
                "writer output must be lint-clean, got: " + m2.warnings.joinIntoString (" | "));

        const auto& lib  = m2.library;
        const auto& mode = lib.modes.getReference (0);

        expectEquals (lib.gainDb, -6.5);
        expect (! lib.polySaveDefault, "polySaveDefault=false must survive");

        // DISABLED loop keeps its authored points (the old writer dropped them).
        const auto& s2 = mode.groups.getReference (0).samples.getReference (1);
        expect (! s2.loop.enabled);
        expect (s2.loop.start.has_value() && *s2.loop.start == 2000, "disabled-loop start must survive");
        expect (s2.loop.end.has_value()   && *s2.loop.end   == 80000, "disabled-loop end must survive");

        const auto& s1 = mode.groups.getReference (0).samples.getReference (0);
        expect (s1.onLoCC64.has_value() && s1.onHiCC64.has_value(), "CC64 range must survive");
        expect (s1.pitchDrift.has_value(), "pitchDrift marker must survive");

        expectEquals (mode.ui.whiteKeyTint, juce::String ("80FFE082"));
        expectEquals (mode.ui.cropTop, 40);
        expectEquals (mode.tags.getReference (0).name, juce::String ("mono-choir"));
        expect (mode.tags.getReference (0).polyphony.has_value());

        const auto& tab = mode.ui.tabs.getReference (0);
        expect (tab.controls.getReference (0).mouseDragSensitivity.has_value());
        expect (! tab.controls.getReference (1).visible, "visible=false must survive");
        expectEquals (tab.controls.getReference (1).bindings.getReference (0).translationTable,
                      juce::String ("0,1;5,4;10,15"));
        expect (tab.controls.getReference (1).bindings.getReference (0).translationReversed);
        expectEquals (tab.menus.getReference (0).hAlign, juce::String ("center"));
        expectEquals (tab.menus.getReference (0).backgroundColor, juce::String ("40000000"));

        expect (! mode.effects.getReference (2).normalizeIr, "normalizeIr=false must survive");
        expectEquals (mode.modulators.getReference (0).bindings.getReference (1).targetId,
                      juce::String ("grp_a"));

        expectEquals (mode.ui.buttonLinks.getReference (1).fromId, juce::String ("btn_dt"));
        expectEquals (mode.ui.buttonLinks.getReference (1).toId,   juce::String ("btn_stereo"));

        expectEquals (mode.ccBindings.getReference (0).targetId, juce::String ("ctl_lfo"));
        expectEquals (mode.sequenceTriggers.getReference (0).transpose, 12);
        expectEquals (mode.menuKeySwitches.getReference (0).option, 1);
    }

    void unknownKeys()
    {
        beginTest ("typo'd keys warn instead of vanishing silently");

        auto r = dm::loadManifestFromJson (R"({
            "schema": 1, "reverbgain": 12,
            "modes": [ { "name": "M",
                "amp": { "attack": 0, "decay": 0, "sustain": 1, "release": 0, "volume": 1, "velTrack": 0 },
                "groups": [ { "samples": [
                    { "source": "flac:x", "loNote": 0, "hiNote": 127, "rootNote": 60,
                      "lenghtFrames": 1000, "pitchKeyTrack": false } ] } ] } ]
        })");
        expect (r.ok, "typos are warnings, not errors");
        expect (r.warnings.joinIntoString ("|").contains ("reverbgain"),
                "root-level typo should be reported");
        expect (r.warnings.joinIntoString ("|").contains ("lenghtFrames"),
                "sample-level typo should be reported");
    }

    void danglingReferences()
    {
        beginTest ("dangling targetId / sequence index / button link warn");

        auto r = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [ { "name": "M",
                "amp": { "attack": 0, "decay": 0, "sustain": 1, "release": 0, "volume": 1, "velTrack": 0 },
                "groups": [ { "uid": "grp_0", "samples": [
                    { "source": "flac:x", "loNote": 0, "hiNote": 127, "rootNote": 60, "pitchKeyTrack": false } ] } ],
                "sequenceTriggers": [ { "note": 24, "sequence": 3 } ],
                "ui": { "width": 100, "height": 100, "tabs": [ { "name": "main",
                    "controls": [ { "id": "ctl_0", "label": "K", "min": 0.0, "max": 1.0, "value": 0.0,
                        "bindings": [ { "type": "effect", "level": "instrument",
                                        "targetId": "fx_nope", "parameter": "FX_MIX" } ] } ],
                    "buttons": [] } ],
                    "buttonLinks": [ { "fromButton": 5, "fromState": 0, "toButton": 0, "toState": 0 } ] } } ]
        })");
        expect (r.ok);
        const auto all = r.warnings.joinIntoString ("|");
        expect (all.contains ("fx_nope"),  "dangling binding targetId should warn");
        expect (all.contains ("sequence"), "out-of-range sequence trigger should warn");
        expect (all.contains ("fromButton"), "out-of-range button link should warn");
    }
};

ManifestLintTests manifestLintTests;
} // namespace
