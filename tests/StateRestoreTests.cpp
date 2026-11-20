// A saved session that holds a value that is not a number, from a damaged or
// hand-edited file. NaN is the one value the parameters' range clamps let
// through, and a NaN master output silenced the instrument.

#include <ManifestPluginProcessor.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstring>
#include <memory>

namespace {

juce::MemoryBlock halfLevelFlac() {
  juce::AudioBuffer<float> buf(1, 48000);
  juce::FloatVectorOperations::fill(buf.getWritePointer(0), 0.5f, 48000);
  juce::MemoryBlock mb;
  juce::FlacAudioFormat flac;
  std::unique_ptr<juce::OutputStream> out =
      std::make_unique<juce::MemoryOutputStream>(mb, false);
  auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                         .withSampleRate(48000.0)
                                         .withNumChannels(1)
                                         .withBitsPerSample(16));
  if (w != nullptr)
    w->writeFromAudioSampleBuffer(buf, 0, 48000);
  return mb;
}

const char *kManifest = R"({
    "schema": 1,
    "modes": [ { "name": "Only",
      "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
      "groups": [ { "samples": [
          { "source":"flac:a", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000 } ] } ] } ]
})";

class StateRestoreTests : public juce::UnitTest {
public:
  StateRestoreTests() : juce::UnitTest("StateRestore", "engine") {}

  void runTest() override {
    beginTest("a value that is not a number is restored as the default");

    const auto flac = halfLevelFlac();
    dm::ManifestPluginProcessor::Assets a;
    a.name = "Test Instrument";
    a.version = "0.0.0";
    a.manifestJson = kManifest;
    a.manifestJsonSize = (int)std::strlen(kManifest);
    a.findResource = [&flac](const juce::String &file,
                             int &size) -> const char * {
      if (file != "a.flac") {
        size = 0;
        return nullptr;
      }
      size = (int)flac.getSize();
      return static_cast<const char *>(flac.getData());
    };
    auto p = std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
    p->setPlayConfigDetails(0, 2, 48000.0, 512);
    p->prepareToPlay(48000.0, 512);
    for (int i = 0; i < 5000 && p->isLoading(); ++i)
      juce::Thread::sleep(1);

    auto xml = p->getApvts().copyState().createXml();
    int poisoned = 0;
    for (auto *child : xml->getChildIterator())
      if (child->hasAttribute("value")) {
        child->setAttribute("value", "nan");
        ++poisoned;
      }
    expect(poisoned > 0, "the state carried values to poison");

    juce::MemoryBlock data;
    juce::AudioProcessor::copyXmlToBinary(*xml, data);
    p->setStateInformation(data.getData(), (int)data.getSize());

    for (auto *parameter : p->getParameters()) {
      const auto value = parameter->getValue();
      expect(std::isfinite(value), parameter->getName(64) + " is a number");
      expectWithinAbsoluteError(value, parameter->getDefaultValue(), 1.0e-6f,
                                parameter->getName(64) + " is its default");
    }

    // Still plays, rather than merely staying finite while silent.
    juce::AudioBuffer<float> buffer(2, 512);
    float peak = 0.0f;
    bool finite = true;
    for (int block = 0; block < 20; ++block) {
      juce::MidiBuffer midi;
      if (block == 1)
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
      buffer.clear();
      p->processBlock(buffer, midi);
      for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 512; ++i) {
          const float v = buffer.getSample(ch, i);
          finite = finite && std::isfinite(v);
          peak = juce::jmax(peak, std::abs(v));
        }
    }
    expect(finite, "the output stays finite");
    expect(peak > 0.1f, "and the note sounds, peak " + juce::String(peak));
  }
};

StateRestoreTests stateRestoreTests;
} // namespace
