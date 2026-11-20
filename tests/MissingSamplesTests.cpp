// The notice a plugin shows when some of its sounds cannot be found. Without
// it, a plugin whose samples.pak is missing loads, shows its face and plays
// silence, which looks like a broken plugin rather than a broken install.

#include <ManifestPluginProcessor.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cstring>

namespace {

juce::MemoryBlock tinyFlac() {
  juce::AudioBuffer<float> buf(1, 256);
  buf.clear();
  juce::MemoryBlock mb;
  juce::FlacAudioFormat flac;
  std::unique_ptr<juce::OutputStream> out =
      std::make_unique<juce::MemoryOutputStream>(mb, false);
  auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                         .withSampleRate(48000.0)
                                         .withNumChannels(1)
                                         .withBitsPerSample(16));
  if (w != nullptr)
    w->writeFromAudioSampleBuffer(buf, 0, 256);
  return mb;
}

const char *kManifest = R"({
    "schema": 1,
    "modes": [ { "name": "Only",
      "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
      "groups": [ { "samples": [
          { "source":"flac:a", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000 },
          { "source":"flac:b", "loNote":61, "hiNote":61, "rootNote":61, "sampleRate":48000 } ] } ] } ]
})";

class MissingSamplesTests : public juce::UnitTest {
public:
  MissingSamplesTests() : juce::UnitTest("MissingSamples", "engine") {}

  void runTest() override {
    const auto flac = tinyFlac();
    const auto folder =
        juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("dmse-missing-samples-test");
    folder.deleteRecursively(); // the pack must not exist

    auto processorWith = [&](juce::StringArray present) {
      dm::ManifestPluginProcessor::Assets a;
      a.name = "Test Instrument";
      a.version = "0.0.0";
      a.manifestJson = kManifest;
      a.manifestJsonSize = (int)std::strlen(kManifest);
      a.packFile = folder.getChildFile("samples.pak");
      a.findResource = [present, &flac](const juce::String &file,
                                        int &size) -> const char * {
        if (!present.contains(file)) {
          size = 0;
          return nullptr;
        }
        size = (int)flac.getSize();
        return static_cast<const char *>(flac.getData());
      };
      return std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
    };

    beginTest("a missing sound gives a notice that says how many and where");
    {
      auto p = processorWith({"a.flac"});
      const auto notice = p->missingSamplesNotice();
      expect(notice.startsWith("Samples not found\n"), notice);
      expect(notice.contains("1 of 2 sounds"), notice);
      expect(notice.contains("Reinstall Test Instrument"), notice);
      expect(notice.contains(folder.getFullPathName()), notice);
    }

    beginTest("no notice when every sound is there");
    {
      auto p = processorWith({"a.flac", "b.flac"});
      expect(p->missingSamplesNotice().isEmpty(), p->missingSamplesNotice());
    }
  }
};

MissingSamplesTests missingSamplesTests;
} // namespace
