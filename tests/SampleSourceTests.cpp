// Sample-source backend tests — the memory-mapped pack + lazy per-mode decode path.
//
// This is how the large libraries ship: samples live in an external, memory-mapped
// samples.pak (not compiled into the binary), and each is decoded to PCM only when a
// mode acquires it and freed when the last reference is released. So a multi-GB library
// costs a small binary, no launch-time decode, and only the ACTIVE mode's PCM resident.
// These properties (lazy decode, refcounted free) are exactly what the tests below pin
// down — previously the whole pack/acquire/release mechanism had no coverage.

#include <audio/EmbeddedFlacSource.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr double kSR = 48000.0;

// A mono FLAC blob holding a constant DC level, so the decoded value is trivial to check.
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

class SampleSourceTests : public juce::UnitTest
{
public:
    SampleSourceTests() : juce::UnitTest ("SampleSource", "engine") {}

    void runTest() override
    {
        beginTest ("memory-mapped pack: lazy decode + refcounted release");

        auto a = dcFlac (0.50f, 1000);
        auto b = dcFlac (0.25f, 1500);
        expect (a.getSize() > 0 && b.getSize() > 0, "fixtures should encode");

        // Concatenate the FLAC blobs into a pack file and write the {id,o,l} index — the
        // exact on-disk layout the converter emits and the plugin memory-maps.
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dmse_pack_test");
        dir.deleteRecursively();
        dir.createDirectory();
        auto pak = dir.getChildFile ("samples.pak");
        auto idx = dir.getChildFile ("samples.pak.json");

        {
            juce::MemoryBlock all;
            all.append (a.getData(), a.getSize());
            all.append (b.getData(), b.getSize());
            expect (pak.replaceWithData (all.getData(), all.getSize()));
        }
        juce::String json;
        json << "[{\"id\":\"flac:a\",\"o\":0,\"l\":" << (int) a.getSize() << "},"
             <<  "{\"id\":\"flac:b\",\"o\":" << (int) a.getSize() << ",\"l\":" << (int) b.getSize() << "}]";
        idx.replaceWithText (json, false, false, "\n");

        dm::EmbeddedFlacSource src;
        expect (src.openPack (pak, idx), "pack should open");

        // Registered from the index, but NOT decoded yet — the low-RAM property.
        expect (src.isRegistered ("flac:a"));
        expect (src.isRegistered ("flac:b"));
        expect (! src.isRegistered ("flac:missing"));
        expect (src.get ("flac:a") == nullptr, "a packed sample must not be decoded before acquire");

        // acquire() decodes on demand and returns the correct PCM.
        const auto* buf = src.acquire ("flac:a");
        expect (buf != nullptr, "acquire should decode the slice");
        if (buf != nullptr)
        {
            expectEquals (buf->getNumFrames(), 1000);
            expectWithinAbsoluteError (buf->audio.getSample (0, 10), 0.50f, 0.01f);
        }
        expect (src.get ("flac:a") != nullptr, "decoded and resident after acquire");
        expect (src.get ("flac:b") == nullptr, "an unacquired sample stays undecoded (only active-mode RAM)");

        // Reference counting: a second acquire pins again; one release keeps it resident,
        // the last release frees the PCM.
        src.acquire ("flac:a");
        src.release ("flac:a");
        expect (src.get ("flac:a") != nullptr, "still resident while a reference remains");
        src.release ("flac:a");
        expect (src.get ("flac:a") == nullptr, "freed when the last reference is released");

        // The compressed ref is retained, so re-acquire re-decodes from the mapping.
        expect (src.acquire ("flac:a") != nullptr, "re-acquire re-decodes after free");
        src.release ("flac:a");

        beginTest ("pack open is robust to bad inputs");
        {
            dm::EmbeddedFlacSource bad;
            expect (! bad.openPack (dir.getChildFile ("does-not-exist.pak"), idx),
                    "missing data file → false");
            auto badIdx = dir.getChildFile ("bad.json");
            badIdx.replaceWithText ("this is not json", false, false, "\n");
            expect (! bad.openPack (pak, badIdx), "unparseable index → false");
        }

        dir.deleteRecursively();
    }
};

SampleSourceTests sampleSourceTests;

} // namespace
