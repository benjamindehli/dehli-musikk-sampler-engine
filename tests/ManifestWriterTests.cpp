// Round-trip tests for the manifest writer (B1).
//
// Strategy: load a fixture → model m1 → write text1 → reload → model m2 → write
// text2. Since both texts are writer output for equivalent models, they must be
// byte-identical (serialization is deterministic). This proves loader/writer agree
// on the whole schema without field-by-field comparison.

#include <model/ManifestLoader.h>
#include <model/ManifestWriter.h>
#include <juce_core/juce_core.h>

namespace
{
juce::File writerFixturesDir()
{
   #if defined (DMSE_TEST_FIXTURES_DIR)
    return juce::File (DMSE_TEST_FIXTURES_DIR);
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/fixtures");
   #endif
}

class ManifestWriterTests : public juce::UnitTest
{
public:
    ManifestWriterTests() : juce::UnitTest ("ManifestWriter", "manifest") {}

    void runTest() override
    {
        for (auto* fixtureName : { "bass.json", "drums.json", "wurli.json", "autostrum.json", "kitchensink.json" })
            roundTrip (fixtureName);
    }

    void roundTrip (const juce::String& fixtureName)
    {
        beginTest ("round-trip " + fixtureName);

        auto file = writerFixturesDir().getChildFile (fixtureName);
        expect (file.existsAsFile(), "fixture not found: " + file.getFullPathName());

        auto m1 = dm::loadManifestFromJson (file.loadFileAsString());
        expect (m1.ok, fixtureName + " should load");

        const auto text1 = dm::writeManifestToJson (m1.library);

        auto m2 = dm::loadManifestFromJson (text1);
        expect (m2.ok, "writer output should re-load: " + m2.errors.joinIntoString ("; "));

        const auto text2 = dm::writeManifestToJson (m2.library);
        expect (text1 == text2, "serialization should be idempotent for " + fixtureName);

        // Sanity: a concrete value survives the trip.
        expect (m2.library.modes.size() == m1.library.modes.size());
        if (m1.library.modes.size() > 0)
            expectEquals (m2.library.modes.getReference (0).name,
                          m1.library.modes.getReference (0).name);
    }
};

ManifestWriterTests manifestWriterTests;
} // namespace
