// Unit tests for the M3 FX chain.
//
// Deterministic tests for the lowpass (attenuates a Nyquist-rate signal, bypasses
// when disabled) and chain configuration. Convolution output is not asserted here
// — JUCE's Convolution loads its IR asynchronously, so it's exercised audibly in
// the plugin (dev controls) rather than in a timing-sensitive unit test.

#include <audio/FxChain.h>
#include <model/Manifest.h>
#include <juce_core/juce_core.h>

namespace
{
constexpr double kSR = 48000.0;
constexpr int kBlock = 1024;

struct NullSource : public dm::SampleSource
{
    const dm::SampleBuffer* get (const juce::String&) const override { return nullptr; }
};

void fillNyquist (juce::AudioBuffer<float>& b)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* w = b.getWritePointer (ch);
        for (int i = 0; i < b.getNumSamples(); ++i)
            w[i] = (i % 2 == 0) ? 1.0f : -1.0f; // alternating = Nyquist
    }
}

class FxChainTests : public juce::UnitTest
{
public:
    FxChainTests() : juce::UnitTest ("FxChain", "audio") {}

    void runTest() override
    {
        testLowpass();
        testBypassAndConfig();
    }

    void testLowpass()
    {
        beginTest ("lowpass attenuates highs, bypasses when disabled");

        dm::Effect lp;
        lp.type = "lowpass";
        lp.enabled = true;
        lp.frequency = 500.0;
        juce::Array<dm::Effect> effects;
        effects.add (lp);

        NullSource src;
        dm::FxChain fx;
        fx.prepare (kSR, kBlock, 1);
        fx.setEffects (effects, src);
        expectEquals (fx.getNumEffects(), 1);

        juce::AudioBuffer<float> buf (1, kBlock);
        fillNyquist (buf);
        fx.process (buf);
        // Measure the settled second half (avoid the filter's startup transient).
        const float filtered = buf.getMagnitude (0, kBlock / 2, kBlock / 2);
        expect (filtered < 0.1f, "Nyquist signal should be strongly attenuated, got "
                                 + juce::String (filtered));

        // Disabled → exact passthrough.
        fx.setEffectEnabled (0, false);
        fillNyquist (buf);
        fx.process (buf);
        expectWithinAbsoluteError (buf.getMagnitude (0, kBlock / 2, kBlock / 2), 1.0f, 1.0e-6f);
    }

    void testBypassAndConfig()
    {
        beginTest ("chain configuration + all-passthrough leaves audio untouched");

        dm::Effect lp;  lp.type = "lowpass";     lp.enabled = false; lp.frequency = 800.0;
        dm::Effect cv;  cv.type = "convolution"; cv.enabled = true;  cv.ir = "ir:none"; cv.wet = 0.0;
        juce::Array<dm::Effect> effects;
        effects.add (lp);
        effects.add (cv);

        NullSource src;
        dm::FxChain fx;
        fx.prepare (kSR, kBlock, 1);
        fx.setEffects (effects, src);
        expectEquals (fx.getNumEffects(), 2);

        // lowpass disabled + convolution wet=0 (and IR missing) → output == input.
        juce::AudioBuffer<float> buf (1, kBlock);
        fillNyquist (buf);
        fx.process (buf);
        expectWithinAbsoluteError (buf.getMagnitude (0, 0, kBlock), 1.0f, 1.0e-6f);

        // Runtime enable + cutoff change should now attenuate.
        fx.setEffectEnabled (0, true);
        fx.setEffectParam (0, "FX_FILTER_FREQUENCY", 400.0f);
        fillNyquist (buf);
        fx.process (buf);
        expect (buf.getMagnitude (0, kBlock / 2, kBlock / 2) < 0.1f,
                "enabling lowpass at runtime should attenuate");
    }
};

FxChainTests fxChainTests;
} // namespace
