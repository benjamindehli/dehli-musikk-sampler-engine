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
#include <audio/EmbeddedFlacSource.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr double kSR = 48000.0;

// A mono FLAC blob holding a constant DC level — a stand-in sample whose rendered
// value we can read straight back to observe what the FX chain did to it.
juce::MemoryBlock dcFlac (float level, int frames)
{
    juce::AudioBuffer<float> buf (1, frames);
    juce::FloatVectorOperations::fill (buf.getWritePointer (0), level, frames);
    juce::MemoryBlock mb;
    juce::FlacAudioFormat flac;
    auto* out = new juce::MemoryOutputStream (mb, false);
    std::unique_ptr<juce::AudioFormatWriter> w (flac.createWriterFor (out, kSR, 1, 16, {}, 0));
    if (w == nullptr) { delete out; return {}; }
    w->writeFromAudioSampleBuffer (buf, 0, frames);
    w.reset();
    return mb;
}

void waitForBuild (dm::SamplerEngine& engine)
{
    for (int i = 0; i < 5000 && engine.isLoading(); ++i)
        juce::Thread::sleep (1);
}

float renderNote (dm::SamplerEngine& engine, int note)
{
    juce::AudioBuffer<float> out (1, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
    engine.processBlock (out, midi, nullptr);
    return out.getSample (0, 200);
}

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

        testGroupEffectByIdRoute();
    }

    // A group-effect binding addressed purely by the effect's id (no positional
    // effectIndex) must route to the right slot inside the right group's chain. The
    // group has a transparent lowpass in slot 0 and a gain in slot 1; a LEVEL binding
    // that names the gain by id must attenuate the signal. If the id failed to resolve,
    // the fallback slot (0, the lowpass) ignores LEVEL and the signal stays untouched —
    // so an attenuated result proves the id resolved to the gain in slot 1.
    void testGroupEffectByIdRoute()
    {
        beginTest ("group-effect binding resolves the effect slot by id");

        auto flac = dcFlac (0.50f, 4000);
        dm::EmbeddedFlacSource source;
        expect (source.addFlac ("flac:a", flac.getData(), flac.getSize()));

        auto parsed = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [
                { "name": "GrpFx",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                  "groups": [
                      { "uid":"g0",
                        "effects": [
                            { "type":"lowpass", "id":"g0_fx_lowpass", "frequency":18000.0 },
                            { "type":"gain",    "id":"g0_fx_gain",    "gain":0.0 } ],
                        "samples": [
                            { "source":"flac:a", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] } ],
                  "ui": { "width":400, "height":200, "tabs": [ { "name":"main",
                      "controls": [
                          { "id":"ctl_0", "label":"gaintrim", "controlIndex":0, "min":0.0, "max":1.0, "value":0.0,
                            "bindings": [
                                { "type":"effect", "level":"group", "parameter":"LEVEL", "targetId":"g0_fx_gain",
                                  "translation":"linear", "translationOutputMin":0.0, "translationOutputMax":-48.0 } ] } ] } ] }
                }
            ]
        })");
        expect (parsed.ok, "group-effect manifest should load");

        dm::SamplerEngine engine;
        engine.prepare (kSR, 512, 1);
        engine.setLibrary (parsed.library, source);
        waitForBuild (engine);

        // Baseline: gain at 0 dB (unity), lowpass transparent to DC → sample passes.
        expectWithinAbsoluteError (renderNote (engine, 60), 0.50f, 0.03f);

        DummyProcessor proc;
        juce::AudioProcessorValueTreeState apvts (
            proc, nullptr, "PARAMS", dm::params::createLayout (parsed.library));
        auto* knob = apvts.getParameter ("ctl_gaintrim");
        expect (knob != nullptr, "group-effect knob should have a param");
        if (knob == nullptr)
            return;

        const auto& mode = parsed.library.modes.getReference (0);
        dm::params::CompiledMode cm (mode, apvts);

        // Knob to max → LEVEL binding drives the gain slot to -48 dB.
        knob->setValueNotifyingHost (1.0f);
        cm.apply (engine, nullptr);
        expect (renderNote (engine, 60) < 0.05f,
                "id-resolved group-effect LEVEL should attenuate the gain slot");
    }
};

CompiledModeTests compiledModeTests;

} // namespace
