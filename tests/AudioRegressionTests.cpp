// Golden-file audio regression tests.
//
// The engine's DSP is data-driven and shared across every plugin, so a change in
// the voice engine, envelope, resampler, FX chain or modulators can silently alter
// the sound of all 13 products at once — something the other unit tests, which check
// structure and single values, do not catch. These tests render a handful of fully
// deterministic synthetic scenarios (hand-authored manifests + generated tones, no
// paid samples, no randomness) through the real SamplerEngine and compare the output
// against a committed reference buffer within a tolerance.
//
// Updating a reference after an intended change: set DMSE_UPDATE_GOLDEN=1 in the
// environment and run the suite once — every scenario rewrites its golden file (the
// same happens automatically for a missing reference, so the first run bootstraps
// them). Commit the regenerated files. The references are small raw float buffers
// under tests/golden/, generated on the release platform.

#include <SamplerEngine.h>
#include <audio/EmbeddedFlacSource.h>
#include <model/ManifestLoader.h>
#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
constexpr double kSR    = 48000.0;
constexpr int    kBlock = 512;

// Per-sample tolerance. Far above float / cross-platform rounding noise (~1e-6, plus a
// little IIR drift), yet far below any audible regression (a dropped effect, wrong gain
// or altered envelope shifts samples by 0.01+), so it flags real changes without being
// flaky across arm64 / x86_64.
constexpr float  kTol   = 2.0e-3f;

juce::File goldenDir()
{
   #if defined (DMSE_TEST_GOLDEN_DIR)
    return juce::File (DMSE_TEST_GOLDEN_DIR);
   #else
    return juce::File::getCurrentWorkingDirectory().getChildFile ("tests/golden");
   #endif
}

// A mono FLAC blob of summed sine partials (24-bit, so decode quantisation is ~1e-7).
juce::MemoryBlock toneFlac (int frames, std::initializer_list<std::pair<double, float>> partials)
{
    juce::AudioBuffer<float> buf (1, frames);
    buf.clear();
    auto* d = buf.getWritePointer (0);
    for (const auto& p : partials)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * p.first / kSR;
        for (int i = 0; i < frames; ++i)
            d[i] += p.second * (float) std::sin (w * (double) i);
    }

    juce::MemoryBlock mb;
    juce::FlacAudioFormat flac;
    auto* out = new juce::MemoryOutputStream (mb, false);
    std::unique_ptr<juce::AudioFormatWriter> writer (flac.createWriterFor (out, kSR, 1, 24, {}, 0));
    if (writer == nullptr) { delete out; return {}; }
    writer->writeFromAudioSampleBuffer (buf, 0, frames);
    writer.reset();
    return mb;
}

void waitForBuild (dm::SamplerEngine& engine)
{
    for (int i = 0; i < 5000 && engine.isLoading(); ++i)
        juce::Thread::sleep (1);
}

class AudioRegressionTests : public juce::UnitTest
{
public:
    AudioRegressionTests() : juce::UnitTest ("AudioRegression", "engine") {}

    // Build the engine from a manifest + named tone samples, then render `blocks`
    // blocks, letting the caller inject MIDI per block. Returns the whole mono render.
    juce::AudioBuffer<float> render (const char* manifestJson,
                                     const std::vector<std::pair<juce::String, juce::MemoryBlock>>& samples,
                                     int blocks,
                                     const std::function<void (int, juce::MidiBuffer&)>& midiFor)
    {
        auto parsed = dm::loadManifestFromJson (manifestJson);
        expect (parsed.ok, "scenario manifest should load");

        dm::EmbeddedFlacSource source;
        for (const auto& s : samples)
            expect (source.addFlac (s.first, s.second.getData(), s.second.getSize()));

        dm::SamplerEngine engine;
        engine.prepare (kSR, kBlock, 1);
        engine.setLibrary (parsed.library, source);
        waitForBuild (engine);

        juce::AudioBuffer<float> full (1, blocks * kBlock);
        full.clear();
        for (int b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> blk (1, kBlock);
            blk.clear();
            juce::MidiBuffer midi;
            if (midiFor) midiFor (b, midi);
            engine.processBlock (blk, midi, nullptr);
            full.copyFrom (0, b * kBlock, blk, 0, 0, kBlock);
        }
        return full;
    }

    // Compare the render against tests/golden/<name>.f32, or (re)write it when the file
    // is missing or DMSE_UPDATE_GOLDEN is set.
    void checkGolden (const juce::String& goldenName, const juce::AudioBuffer<float>& buf)
    {
        auto file = goldenDir().getChildFile (goldenName + ".f32");
        const int nCh = buf.getNumChannels();
        const int nSm = buf.getNumSamples();

        const bool update = juce::SystemStats::getEnvironmentVariable ("DMSE_UPDATE_GOLDEN", {}).isNotEmpty();
        if (update || ! file.existsAsFile())
        {
            file.getParentDirectory().createDirectory();
            juce::MemoryBlock mb;
            {
                juce::MemoryOutputStream os (mb, false);
                os.writeInt (nCh);
                os.writeInt (nSm);
                for (int c = 0; c < nCh; ++c)
                    os.write (buf.getReadPointer (c), sizeof (float) * (size_t) nSm);
            }
            expect (file.replaceWithData (mb.getData(), mb.getSize()),
                    "could not write golden: " + file.getFullPathName());
            logMessage ("wrote golden reference: " + file.getFullPathName());
            return;
        }

        juce::MemoryBlock mb;
        expect (file.loadFileAsData (mb), "golden unreadable: " + goldenName);
        juce::MemoryInputStream is (mb, false);
        const int gCh = is.readInt();
        const int gSm = is.readInt();
        expectEquals (gCh, nCh, "golden channel count mismatch: " + goldenName);
        expectEquals (gSm, nSm, "golden sample count mismatch: " + goldenName);
        if (gCh != nCh || gSm != nSm)
            return;

        float maxDiff = 0.0f;
        for (int c = 0; c < nCh; ++c)
        {
            const float* r = buf.getReadPointer (c);
            for (int i = 0; i < nSm; ++i)
            {
                float g = 0.0f;
                is.read (&g, sizeof (float));
                maxDiff = juce::jmax (maxDiff, std::abs (g - r[i]));
            }
        }
        expect (maxDiff <= kTol,
                goldenName + " diverged from golden (max abs diff " + juce::String (maxDiff, 6)
                + " > tolerance " + juce::String (kTol, 6) + ")");
        logMessage (goldenName + ": max abs diff from golden = " + juce::String (maxDiff, 8));
    }

    void runTest() override
    {
        beginTest ("voice + envelope + pitch resampling");
        {
            std::vector<std::pair<juce::String, juce::MemoryBlock>> samples;
            samples.emplace_back ("flac:tone", toneFlac (40000, { { 330.0, 0.6f }, { 990.0, 0.3f } }));

            // Played at note 67 against rootNote 60 → the sinc resampler shifts it up;
            // a real ADSR then shapes attack/decay/sustain and the release tail.
            auto buf = render (R"({
                "schema": 1,
                "modes": [
                    { "name": "AdsrPitch",
                      "amp": { "attack":0.02, "decay":0.10, "sustain":0.5, "release":0.20, "volume":1, "velTrack":0 },
                      "groups": [{ "samples": [
                          { "source":"flac:tone", "loNote":48, "hiNote":72, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":true } ] }] }
                ]
            })", samples, 30, [] (int b, juce::MidiBuffer& m)
            {
                if (b == 0)  m.addEvent (juce::MidiMessage::noteOn  (1, 67, 0.9f), 0);
                if (b == 15) m.addEvent (juce::MidiMessage::noteOff (1, 67), 0);
            });
            checkGolden ("voice_adsr_pitch", buf);
        }

        beginTest ("FX chain: lowpass + gain + wave shaper");
        {
            std::vector<std::pair<juce::String, juce::MemoryBlock>> samples;
            samples.emplace_back ("flac:tone", toneFlac (16000, { { 330.0, 0.6f }, { 1980.0, 0.3f } }));

            // Sustained tone through the instrument FX chain: the lowpass attenuates the
            // 1980 Hz partial, the gain trims level, the wave shaper adds tanh saturation.
            auto buf = render (R"({
                "schema": 1,
                "modes": [
                    { "name": "Fx",
                      "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                      "groups": [{ "samples": [
                          { "source":"flac:tone", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] }],
                      "effects": [
                          { "type":"lowpass",     "id":"fx_lowpass", "frequency":1500.0, "resonance":0.3, "enabled":true },
                          { "type":"gain",        "id":"fx_gain",    "gain":-3.0,        "enabled":true },
                          { "type":"wave_shaper", "id":"fx_ws",      "drive":6.0,        "outputLevel":0.5, "enabled":true } ] }
                ]
            })", samples, 20, [] (int b, juce::MidiBuffer& m)
            {
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            });
            checkGolden ("fxchain_lowpass_gain_waveshaper", buf);
        }

        beginTest ("group FX modulated by an LFO (tremulant)");
        {
            std::vector<std::pair<juce::String, juce::MemoryBlock>> samples;
            samples.emplace_back ("flac:tone", toneFlac (40000, { { 330.0, 0.7f } }));

            // A sine LFO sweeps a per-group gain slot (addressed by effect id) — the
            // Elektrisk-style tremulant, exercising group chains + the modulator path.
            auto buf = render (R"({
                "schema": 1,
                "modes": [
                    { "name": "Trem",
                      "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
                      "groups": [
                          { "uid":"g0",
                            "effects": [
                                { "type":"lowpass", "id":"g0_fx_lowpass", "frequency":18000.0 },
                                { "type":"gain",    "id":"g0_fx_gain",    "gain":0.0 } ],
                            "samples": [
                                { "source":"flac:tone", "loNote":60, "hiNote":60, "rootNote":60, "sampleRate":48000, "pitchKeyTrack":false } ] } ],
                      "modulators": [
                          { "id":"mod_0", "shape":"sine", "frequency":5.0, "modAmount":1.0,
                            "bindings": [
                                { "type":"effect", "level":"group", "parameter":"LEVEL", "targetId":"g0_fx_gain",
                                  "modBehavior":"set", "translation":"linear",
                                  "translationOutputMin":0.0, "translationOutputMax":-12.0 } ] } ] }
                ]
            })", samples, 30, [] (int b, juce::MidiBuffer& m)
            {
                if (b == 0) m.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            });
            checkGolden ("group_fx_lfo_tremulant", buf);
        }
    }
};

AudioRegressionTests audioRegressionTests;

} // namespace
