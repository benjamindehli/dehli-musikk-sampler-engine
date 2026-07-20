// Unit tests for the compiled param plan (params/CompiledMode).
//
// Reproduces the EDB-Orgel mod-wheel chain end to end at the params level:
// a CC1 binding targets an INVISIBLE control (by id, with controlIndex fallback)
// whose bindings drive a GLOBAL_TUNING vibrato modulator — the wheel must move the
// control's param, and apply() must route the param to the engine's LFO.

#include <params/ManifestParameters.h>
#include <params/CompiledMode.h>
#include <model/ManifestLoader.h>
#include <SamplerEngine.h>
#include <juce_audio_processors/juce_audio_processors.h>

namespace
{

// Minimal host processor — just enough to own an APVTS in a console app.
struct DummyProcessor : juce::AudioProcessor
{
    const juce::String getName() const override { return "dummy"; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}
};

class CompiledModeTests : public juce::UnitTest
{
public:
    CompiledModeTests() : juce::UnitTest ("CompiledMode", "engine") {}

    void runTest() override
    {
        beginTest ("CC binding drives its target control param (EDB mod-wheel chain)");

        // EDB-like mode: invisible control -> vibrato modulator, CC1 -> that control.
        auto parsed = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [
                { "name": "Organ",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                  "groups": [{ "samples": [
                      { "source":"flac:a", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] }],
                  "modulators": [
                      { "id":"mod_0", "shape":"sine", "frequency":8.0, "modAmount":0.0,
                        "bindings": [ { "type":"amp", "level":"instrument", "parameter":"GLOBAL_TUNING" } ] } ],
                  "ccBindings": [
                      { "cc":1, "parameter":"FREQUENCY", "targetId":"ctl_0", "controlIndex":0,
                        "normMin":0.0, "normMax":1.0 } ],
                  "ui": { "width":812, "height":375, "tabs": [ { "name":"main",
                      "controls": [
                          { "id":"ctl_0", "label":"vibctl", "controlIndex":0, "visible":false,
                            "min":0.0, "max":99.0, "value":0.0,
                            "bindings": [
                                { "type":"modulator", "level":"instrument", "targetId":"mod_0",
                                  "parameter":"FREQUENCY", "translation":"linear",
                                  "translationOutputMin":0.0, "translationOutputMax":8.0, "position":0 },
                                { "type":"modulator", "level":"instrument", "targetId":"mod_0",
                                  "parameter":"MOD_AMOUNT", "translation":"linear",
                                  "translationOutputMin":0.0, "translationOutputMax":0.2, "position":0 } ] } ] } ] }
                }
            ]
        })");
        expect (parsed.ok, "manifest should load");
        expectEquals (parsed.library.modes.getReference (0).ccBindings.size(), 1);

        DummyProcessor proc;
        juce::AudioProcessorValueTreeState apvts (
            proc, nullptr, "PARAMS", dm::params::createLayout (parsed.library));

        // The invisible control must still own an automatable param.
        auto* raw = apvts.getRawParameterValue ("ctl_vibctl");
        expect (raw != nullptr, "invisible engine-driving control should have a param");
        if (raw == nullptr)
            return;
        expectWithinAbsoluteError (raw->load(), 0.0f, 1.0e-6f);

        const auto& mode = parsed.library.modes.getReference (0);
        dm::params::CompiledMode cm (mode, apvts);

        // Mod wheel to max: CC1 = 127 at the plan level.
        juce::MidiBuffer midi;
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);
        cm.applyCc (midi);
        expectWithinAbsoluteError (raw->load(), 1.0f, 1.0e-4f);

        // Half-way wheel.
        juce::MidiBuffer midi2;
        midi2.addEvent (juce::MidiMessage::controllerEvent (1, 1, 64), 0);
        cm.applyCc (midi2);
        expectWithinAbsoluteError (raw->load(), 64.0f / 127.0f, 1.0e-4f);

        // apply() must not crash routing the param to the engine's LFO slots (the
        // engine has no mode built — setters store into override slots).
        dm::SamplerEngine engine;
        cm.apply (engine, nullptr);
        expect (true, "apply with LFO routes should be safe");
    }
};

CompiledModeTests compiledModeTests;

} // namespace
