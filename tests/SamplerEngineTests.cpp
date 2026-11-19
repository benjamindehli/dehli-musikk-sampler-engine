// Unit tests for SamplerEngine mode switching (M5).
//
// Single-threaded exercise of the build-and-swap path: setActiveMode publishes
// a new mode, the next processBlock adopts it. Two modes map note 60 to samples
// of different levels, so we can confirm which mode is live by the rendered
// value.

#include <SamplerEngine.h>
#include <audio/EmbeddedFlacSource.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <model/ManifestLoader.h>

namespace {
constexpr double kSR = 48000.0;

juce::MemoryBlock dcFlac(float level, int frames) {
  juce::AudioBuffer<float> buf(1, frames);
  juce::FloatVectorOperations::fill(buf.getWritePointer(0), level, frames);

  juce::MemoryBlock mb;
  juce::FlacAudioFormat flac;
  std::unique_ptr<juce::OutputStream> out =
      std::make_unique<juce::MemoryOutputStream>(mb, false);
  auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                         .withSampleRate(kSR)
                                         .withNumChannels(1)
                                         .withBitsPerSample(16));
  if (w == nullptr)
    return {};
  w->writeFromAudioSampleBuffer(buf, 0, frames);
  w.reset();
  return mb;
}

float renderNote(dm::SamplerEngine &engine, int note) {
  juce::AudioBuffer<float> out(1, 512);
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
  engine.processBlock(out, midi, nullptr);
  return out.getSample(0, 200);
}

// Modes build ASYNCHRONOUSLY (setLibrary/setActiveMode start a background
// decode; the engine renders silence until the next processBlock adopts the
// published unit). Real hosts see the loading overlay; the test must wait for
// the build before rendering.
void waitForBuild(dm::SamplerEngine &engine) {
  for (int i = 0; i < 5000 && engine.isLoading(); ++i)
    juce::Thread::sleep(1);
}

class SamplerEngineTests : public juce::UnitTest {
public:
  SamplerEngineTests() : juce::UnitTest("SamplerEngine", "engine") {}

  void runTest() override {
    beginTest("mode switching: build + lock-free adopt");

    auto flacA = dcFlac(0.50f, 2000);
    auto flacB = dcFlac(0.25f, 2000);

    dm::EmbeddedFlacSource source;
    expect(source.addFlac("flac:a", flacA.getData(), flacA.getSize()));
    expect(source.addFlac("flac:b", flacB.getData(), flacB.getSize()));

    auto parsed = dm::loadManifestFromJson(R"({
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
    expect(parsed.ok, "two-mode manifest should load");

    dm::SamplerEngine engine;
    engine.prepare(kSR, 512, 1);
    engine.setLibrary(parsed.library, source);
    waitForBuild(engine);

    expectEquals(engine.getNumModes(), 2);
    expectEquals(engine.getModeNames().joinIntoString(","),
                 juce::String("Alpha,Beta"));
    expectEquals(engine.getActiveModeIndex(), 0);

    // Mode 0 (Alpha) → sample A (0.50).
    expectWithinAbsoluteError(renderNote(engine, 60), 0.50f, 0.02f);

    // Switch to mode 1; the next processBlock adopts it → sample B (0.25).
    engine.setActiveMode(1);
    waitForBuild(engine);
    expectEquals(engine.getActiveModeIndex(), 1);
    expectWithinAbsoluteError(renderNote(engine, 60), 0.25f, 0.02f);

    // And back to mode 0.
    engine.setActiveMode(0);
    waitForBuild(engine);
    expectWithinAbsoluteError(renderNote(engine, 60), 0.50f, 0.02f);

    // Out-of-range switch is ignored.
    engine.setActiveMode(99);
    expectEquals(engine.getActiveModeIndex(), 0);

    // Semantic FX setters are safe to call even with no FX in these modes.
    engine.setReverbMix(0.5f);
    engine.setLowpassEnabled(true);
    juce::AudioBuffer<float> out(1, 256);
    juce::MidiBuffer empty;
    engine.processBlock(out, empty, nullptr);
    expect(true,
           "processBlock with FX overrides + no effects should not crash");

    // Alpha and Beta have a release of 0 and no effects, so nothing follows a
    // note-off.
    expectWithinAbsoluteError(engine.getTailLengthSeconds(), 0.0, 1.0e-9);

    tailLength();
  }

  // The tail a host is told: the longest a note rings on after note-off, plus
  // the longest impulse response the mode can load. Sample and IR lengths are
  // all different, so a wrong sum cannot pass by accident.
  void tailLength() {
    beginTest("tail length: longest note release plus longest IR");

    auto oneSecond = dcFlac(0.5f, 48000);  // a release-trigger sample
    auto halfSecond = dcFlac(0.5f, 24000); // a one-shot, keytracked
    auto shortIr = dcFlac(0.1f, 12000);    // 0.25 s
    auto longIr = dcFlac(0.1f, 96000);     // 2 s, only via the menu
    auto sustained = dcFlac(0.5f, 480000); // 10 s, but enveloped

    dm::EmbeddedFlacSource source;
    expect(
        source.addFlac("flac:rel", oneSecond.getData(), oneSecond.getSize()));
    expect(source.addFlac("flac:shot", halfSecond.getData(),
                          halfSecond.getSize()));
    expect(source.addFlac("ir:short", shortIr.getData(), shortIr.getSize()));
    expect(source.addFlac("ir:long", longIr.getData(), longIr.getSize()));
    expect(
        source.addFlac("flac:sus", sustained.getData(), sustained.getSize()));

    auto parsed = dm::loadManifestFromJson(R"({
            "schema": 1,
            "modes": [
                { "name": "Release trigger and a menu IR",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0.3, "volume":1, "velTrack":0 },
                  "groups": [
                      { "samples": [ { "source":"flac:sus", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000 } ] },
                      { "trigger":"release", "samples": [ { "source":"flac:rel", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000 } ] } ],
                  "effects": [ { "type":"convolution", "ir":"ir:short", "mix":0.5 } ],
                  "ui": { "width":400, "height":200, "tabs": [ { "name":"main", "controls": [],
                      "menus": [ { "options": [
                          { "name":"Short", "bindings": [ { "type":"effect", "level":"instrument", "effectIndex":0,
                              "parameter":"FX_IR_FILE", "translation":"fixed_value", "translationValue":"ir:short" } ] },
                          { "name":"Long", "bindings": [ { "type":"effect", "level":"instrument", "effectIndex":0,
                              "parameter":"FX_IR_FILE", "translation":"fixed_value", "translationValue":"ir:long" } ] } ] } ] } ] } },
                { "name": "One-shot an octave down, and a Release knob",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0.1, "volume":1, "velTrack":0 },
                  "groups": [
                      { "ampEnvEnabled": false, "samples": [ { "source":"flac:shot", "loNote":48, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":true } ] } ],
                  "ui": { "width":400, "height":200, "tabs": [ { "name":"main", "controls": [
                      { "id":"ctl_rel", "label":"Release", "controlIndex":0, "min":0.0, "max":1.0, "value":0.2,
                        "bindings": [ { "type":"amp", "level":"instrument", "parameter":"ENV_RELEASE",
                            "translation":"linear", "translationOutputMin":0.05, "translationOutputMax":0.8 } ] } ] } ] } },
                { "name": "Release knob longer than the one-shot",
                  "amp": { "attack":0, "decay":0, "sustain":1, "release":0.1, "volume":1, "velTrack":0 },
                  "groups": [
                      { "samples": [ { "source":"flac:sus", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000 } ] } ],
                  "effects": [ { "type":"convolution", "ir":"ir:short", "mix":0.5 } ],
                  "ui": { "width":400, "height":200, "tabs": [ { "name":"main", "controls": [
                      { "id":"ctl_rel", "label":"Release", "controlIndex":0, "min":0.0, "max":1.0, "value":0.2,
                        "bindings": [ { "type":"amp", "level":"group", "groupIndex":0, "parameter":"ENV_RELEASE",
                            "translation":"linear", "translationOutputMin":4.0, "translationOutputMax":0.07 } ] } ] } ] } }
            ]
        })");
    expect(parsed.ok,
           "tail manifest should load: " + parsed.errors.joinIntoString("; "));
    if (!parsed.ok)
      return;

    dm::SamplerEngine engine;
    engine.prepare(kSR, 512, 1);
    engine.setLibrary(parsed.library, source);
    waitForBuild(engine);

    // The release-trigger sample (1 s) outlasts the 0.3 s envelope release, and
    // the menu's long IR (2 s) counts even though the short one is loaded. The
    // 10 s sustained sample is cut by its envelope, so it does not count.
    expectWithinAbsoluteError(engine.getTailLengthSeconds(), 1.0 + 2.0, 1.0e-6);

    // A one-shot ignores note-off and plays a 0.5 s sample, an octave slower at
    // its lowest key: 1 s. That beats the Release knob's 0.8 s ceiling. No IR.
    engine.setActiveMode(1);
    waitForBuild(engine);
    expectWithinAbsoluteError(engine.getTailLengthSeconds(), 1.0, 1.0e-6);

    // A group Release knob whose range runs backwards still reaches 4 s, plus
    // the 0.25 s IR.
    engine.setActiveMode(2);
    waitForBuild(engine);
    expectWithinAbsoluteError(engine.getTailLengthSeconds(), 4.0 + 0.25,
                              1.0e-6);

    // The value is published with the build, before the audio thread adopts
    // the mode, so a host asking between the two already gets the new tail.
    engine.setActiveMode(0);
    waitForBuild(engine);
    expectWithinAbsoluteError(engine.getTailLengthSeconds(), 3.0, 1.0e-6);
  }
};

SamplerEngineTests samplerEngineTests;
} // namespace
