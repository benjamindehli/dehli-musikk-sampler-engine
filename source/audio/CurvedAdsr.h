#pragma once

// dehli-musikk-sampler-engine — amp envelope with curve shaping.
//
// A drop-in replacement for juce::ADSR (same call surface: setSampleRate /
// setParameters / noteOn / noteOff / getNextSample / isActive) that follows
// DecentSampler's curve model. DecentSampler segments are NOT linear: each curve
// is a value in -100..100 where -100 = logarithmic, 0 = linear, 100 = exponential,
// with DEFAULTS attack -100, decay +100, release +100. juce::ADSR is linear-only,
// which makes decays read as longer/softer than the source. This matches the
// source's "fast-then-slow" shape so converted libraries sound like DecentSampler.

#include <cmath>

namespace dm
{

class CurvedAdsr
{
public:
    struct Parameters
    {
        float attack  = 0.0f;   // seconds
        float decay   = 0.0f;
        float sustain = 1.0f;   // level 0..1
        float release = 0.0f;
        // DecentSampler curve defaults (-100 log, 0 linear, 100 exponential).
        float attackCurve  = -100.0f;
        float decayCurve   =  100.0f;
        float releaseCurve  = 100.0f;
    };

    void setSampleRate (double sr) noexcept { sampleRate = sr > 0.0 ? sr : 44100.0; recalc(); }

    void setParameters (const Parameters& p) noexcept { params = p; recalc(); }

    void noteOn() noexcept
    {
        stage = Stage::attack;
        stageSamp = 0;
        segE = 1.0;
        value = 0.0f;
    }

    void noteOff() noexcept
    {
        if (stage != Stage::idle && stage != Stage::release)
        {
            releaseStart = value;
            stage = Stage::release;
            stageSamp = 0;
            segE = 1.0;
        }
    }

    void reset() noexcept { stage = Stage::idle; value = 0.0f; stageSamp = 0; segE = 1.0; }

    /** True once note-off started the release (or the envelope finished) — a
        releasing reed's valve is closed, so it no longer draws air (AirSupply). */
    bool isReleasing() const noexcept { return stage == Stage::release || stage == Stage::idle; }

    bool isActive() const noexcept { return stage != Stage::idle; }

    // The curved segments follow shape(p) = (exp(a·p) − 1) / (exp(a) − 1) with
    // p = stageSamp/len. Rather than paying two exp() calls PER SAMPLE PER VOICE,
    // exp(a·p) is carried in `segE` and advanced by the stage-constant factor
    // exp(a/len) — one multiply per sample, mathematically the same curve. segE is
    // a double so the multiplicative accumulation doesn't drift over long stages.
    float getNextSample() noexcept
    {
        switch (stage)
        {
            case Stage::idle:
                return 0.0f;

            case Stage::attack:
            {
                value = attackLen > 0
                          ? (linA ? (float) stageSamp * invLenA
                                  : (float) ((segE - 1.0) * invDenA))
                          : 1.0f;
                segE *= mulA;
                if (++stageSamp >= attackLen) { stage = Stage::decay; stageSamp = 0; segE = 1.0; }
                return value;
            }
            case Stage::decay:
            {
                const float sh = decayLen > 0
                                   ? (linD ? (float) stageSamp * invLenD
                                           : (float) ((segE - 1.0) * invDenD))
                                   : 1.0f;
                value = 1.0f + (params.sustain - 1.0f) * sh;
                segE *= mulD;
                if (++stageSamp >= decayLen) { stage = Stage::sustain; value = params.sustain; }
                return value;
            }
            case Stage::sustain:
                return value = params.sustain;

            case Stage::release:
            {
                const float sh = releaseLen > 0
                                   ? (linR ? (float) stageSamp * invLenR
                                           : (float) ((segE - 1.0) * invDenR))
                                   : 1.0f;
                value = releaseStart * (1.0f - sh);
                segE *= mulR;
                if (++stageSamp >= releaseLen) { stage = Stage::idle; value = 0.0f; }
                return value;
            }
        }
        return 0.0f;
    }

private:
    enum class Stage { idle, attack, decay, sustain, release };

    // Maps a DecentSampler curve (-100..100) to the bend coefficient `a` used by
    // shape(). Negative `a` = "fast then slow" progress. Attack and decay/release
    // flip sign so that the DS defaults (attack -100, decay/release +100) all land
    // on the same fast-then-slow shape.
    static float curveToA (float curve, bool isAttack) noexcept
    {
        constexpr float kStrength = 4.0f;
        const float k = (curve / 100.0f) * kStrength;
        return isAttack ? k : -k;
    }

    // Precompute one stage's per-sample constants: a≈0 → linear (progress is
    // stageSamp·invLen); otherwise the exp recurrence (segE·mul per sample) with
    // 1/(exp(a)−1) folded into invDen.
    void segmentConsts (float a, int len, bool& lin, float& invLen, double& mul, double& invDen) noexcept
    {
        lin    = a > -1.0e-3f && a < 1.0e-3f;
        invLen = len > 0 ? 1.0f / (float) len : 0.0f;
        mul    = (! lin && len > 0) ? std::exp ((double) a / (double) len) : 1.0;
        invDen = ! lin ? 1.0 / (std::exp ((double) a) - 1.0) : 0.0;
    }

    void recalc() noexcept
    {
        attackLen  = (int) (params.attack  * sampleRate + 0.5);
        decayLen   = (int) (params.decay   * sampleRate + 0.5);
        releaseLen = (int) (params.release * sampleRate + 0.5);
        const float aA = curveToA (params.attackCurve,  true);
        const float aD = curveToA (params.decayCurve,   false);
        const float aR = curveToA (params.releaseCurve, false);
        segmentConsts (aA, attackLen,  linA, invLenA, mulA, invDenA);
        segmentConsts (aD, decayLen,   linD, invLenD, mulD, invDenD);
        segmentConsts (aR, releaseLen, linR, invLenR, mulR, invDenR);
    }

    double sampleRate = 44100.0;
    Parameters params;
    Stage stage = Stage::idle;
    int   stageSamp = 0;
    int   attackLen = 0, decayLen = 0, releaseLen = 0;
    // Per-stage curve constants (recalc) + the running exp accumulator (per segment).
    bool   linA = true, linD = true, linR = true;
    float  invLenA = 0.0f, invLenD = 0.0f, invLenR = 0.0f;
    double mulA = 1.0, mulD = 1.0, mulR = 1.0;
    double invDenA = 0.0, invDenD = 0.0, invDenR = 0.0;
    double segE = 1.0;
    float value = 0.0f;
    float releaseStart = 0.0f;
};

} // namespace dm
