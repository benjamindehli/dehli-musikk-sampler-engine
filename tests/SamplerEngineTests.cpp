// Unit tests for SamplerEngine mode switching (M5).
//
// Single-threaded exercise of the build-and-swap path: setActiveMode publishes a
// new mode, the next processBlock adopts it. Two modes map note 60 to samples of
// different levels, so we can confirm which mode is live by the rendered value.

#include <SamplerEngine.h>
#include <audio/EmbeddedFlacSource.h>
#include <model/ManifestLoader.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr double kSR = 48000.0;

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

float renderNote (dm::SamplerEngine& engine, int note)
{
    juce::AudioBuffer<float> out (1, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);
    engine.processBlock (out, midi, nullptr);
    return out.getSample (0, 200);
}

// Modes build ASYNCHRONOUSLY (setLibrary/setActiveMode start a background decode; the
// engine renders silence until the next processBlock adopts the published unit). Real
// hosts see the loading overlay; the test must wait for the build before rendering.
void waitForBuild (dm::SamplerEngine& engine)
{
    for (int i = 0; i < 5000 && engine.isLoading(); ++i)
        juce::Thread::sleep (1);
}

class SamplerEngineTests : public juce::UnitTest
{
public:
    SamplerEngineTests() : juce::UnitTest ("SamplerEngine", "engine") {}

    void runTest() override
    {
        beginTest ("mode switching: build + lock-free adopt");

        auto flacA = dcFlac (0.50f, 2000);
        auto flacB = dcFlac (0.25f, 2000);

        dm::EmbeddedFlacSource source;
        expect (source.addFlac ("flac:a", flacA.getData(), flacA.getSize()));
        expect (source.addFlac ("flac:b", flacB.getData(), flacB.getSize()));

        auto parsed = dm::loadManifestFromJson (R"({
            "schema": 1,
            "modes": [
                { "name": "Alpha",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                  "groups": [{ "samples": [
                      { "source":"flac:a", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] }] },
                { "name": "Beta",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                  "groups": [{ "samples": [
                      { "source":"flac:b", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] }] }
            ]
        })");
        expect (parsed.ok, "two-mode manifest should load");

        dm::SamplerEngine engine;
        engine.prepare (kSR, 512, 1);
        engine.setLibrary (parsed.library, source);
        waitForBuild (engine);

        expectEquals (engine.getNumModes(), 2);
        expectEquals (engine.getModeNames().joinIntoString (","), juce::String ("Alpha,Beta"));
        expectEquals (engine.getActiveModeIndex(), 0);

        // Mode 0 (Alpha) → sample A (0.50).
        expectWithinAbsoluteError (renderNote (engine, 60), 0.50f, 0.02f);

        // Switch to mode 1; the next processBlock adopts it → sample B (0.25).
        engine.setActiveMode (1);
        waitForBuild (engine);
        expectEquals (engine.getActiveModeIndex(), 1);
        expectWithinAbsoluteError (renderNote (engine, 60), 0.25f, 0.02f);

        // And back to mode 0.
        engine.setActiveMode (0);
        waitForBuild (engine);
        expectWithinAbsoluteError (renderNote (engine, 60), 0.50f, 0.02f);

        // Out-of-range switch is ignored.
        engine.setActiveMode (99);
        expectEquals (engine.getActiveModeIndex(), 0);

        // Semantic FX setters are safe to call even with no FX in these modes.
        engine.setReverbMix (0.5f);
        engine.setLowpassEnabled (true);
        juce::AudioBuffer<float> out (1, 256);
        juce::MidiBuffer empty;
        engine.processBlock (out, empty, nullptr);
        expect (true, "processBlock with FX overrides + no effects should not crash");
    }
};

SamplerEngineTests samplerEngineTests;
} // namespace
