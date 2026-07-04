#pragma once

// dehli-musikk-sampler-engine — polyphonic sample voice engine.
//
// M2 scope (the Bass mode): one dedicated sample per note, fixed-pitch playback
// resampled to the device rate, a per-voice amp ADSR resolved from the mode/group
// envelope, and monophonic tag-choke. Velocity layers, round-robin, loops,
// triggers, sequences and LFOs are represented in the model but not yet
// interpreted here — those arrive in later milestones.

#include "SampleSource.h"
#include "CurvedAdsr.h"
#include "FxChain.h"
#include <model/Manifest.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <vector>

namespace dm
{

class VoiceEngine
{
public:
    VoiceEngine();

    void prepare (double sampleRate, int maxBlockSize, int numChannels);
    void releaseResources();

    /** Configure playback from a manifest mode + a sample source. Builds the
        note→zone map and resolves per-group amp envelopes. The source must
        outlive subsequent processBlock() calls. Safe to call only when not
        rendering (message thread); M5 adds audio-thread-safe swapping. */
    void setMode (const Mode& mode, const SampleSource& source);

    /** Render `buffer.getNumSamples()` frames, mixing active voices and applying
        the MIDI note on/offs at their sample offsets. Clears the buffer first. */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    void allNotesOff();
    int  getActiveVoiceCount() const noexcept;

    // Runtime amp overrides (from UI controls / automation). Negative = "no
    // override, use the mode/group value". Applied to new voices.
    void setAmpAttack (float seconds);
    void setAmpDecay (float seconds);
    void setAmpSustain (float level);
    void setAmpRelease (float seconds);

    // Envelope curve shapes (-100 log … 0 linear … +100 exp). Sentinel = no override.
    void setAmpAttackCurve  (float curve);
    void setAmpDecayCurve   (float curve);
    void setAmpReleaseCurve (float curve);

    // Per-group amp overrides (e.g. an organ "loudness" that lengthens only the reed
    // groups' attack, not the key-noise groups). Take precedence over the global ones.
    void setGroupAmpAttack (int groupIndex, float seconds);
    void setGroupAmpDecay (int groupIndex, float seconds);
    void setGroupAmpSustain (int groupIndex, float level);
    void setGroupAmpRelease (int groupIndex, float seconds);

    /** Per-group output volume multiplier (AMP_VOLUME, e.g. Voice1/Voice2). */
    void setGroupVolume (int groupIndex, float volume);

    /** Per-group tag-addressed volume multiplier (TAG_VOLUME mixer knobs).
        Independent of setGroupVolume — both multiply, so a tag mixer and an
        AMP_VOLUME master can affect the same group without fighting. */
    void setGroupTagVolume (int groupIndex, float volume);

    /** Set modulator `position`'s depth 0..1 (MOD_AMOUNT). 0 = no modulation. The
        modulator's shape/rate/targets come from the mode (setMode). */
    void setLfoDepth (int position, float depth);

    /** Set modulator `position`'s rate in Hz (FREQUENCY / Tremulant Rate). */
    void setLfoRate (int position, float hz);

    /** Enable/disable a group at runtime (group-level ENABLED, e.g. the Keyboard
        mode's Mono/Poly button switching between a mono and a poly group). */
    void setGroupEnabled (int groupIndex, bool enabled);

    /** Pitch-wheel bend range in semitones (default 2). Re-applied per block. */
    void setPitchBendRange (double semitones) { bendRangeSemitones = semitones; }
    void setPitchDriftAmount  (float a) { pitchDriftAmount.store (a); }    // pitch-drift wheel (0..1)
    void setVolumeDriftAmount (float a) { volumeDriftAmount.store (a); }   // volume-drift wheel (0..1)

    /** Per-group pitch offset in semitones (GROUP_TUNING). Applied to new voices. */
    void setGroupTuning (int groupIndex, float semitones);

    /** Set a parameter on one of a group's per-group insert effects (by effectIndex):
        FX_FILTER_FREQUENCY / LEVEL / FX_MIX etc. (e.g. an organ swell filter). */
    void setGroupEffectParam (int groupIndex, int effectIndex, const juce::String& parameter, float value);

    /** Per-group output gain in dB (group-level `gain` effect LEVEL). Independent of
        setGroupVolume — the two multiply, so a tag mixer and a group gain can both
        affect the same group without clobbering each other. */
    void setGroupGain (int groupIndex, float db);

    /** Global amp velocity-tracking override (AMP_VEL_TRACK, 0..1). Negative = use
        each group's own velTrack. How much note velocity scales voice volume. */
    void setAmpVelTrack (float amount) { ovVelTrack = amount; }

private:
    struct Zone
    {
        int loNote = 0, hiNote = 127, rootNote = 60;
        int loVel = 0, hiVel = 127;   // velocity layer (group loVel/hiVel)
        const SampleBuffer* buffer = nullptr;
        bool pitchKeyTrack = false;
        bool releaseTrigger = false;   // group trigger="release": fires on note-off
        CurvedAdsr::Parameters adsr;
        bool ampEnv = true;            // false (ampEnvEnabled=false) → one-shot: full gain, ignores note-off
        float gain = 1.0f;
        float velTrack = 0.0f;
        int groupIndex = -1;
        juce::StringArray tags;
        juce::StringArray silencedByTags;

        // Round-robin: zones sharing a groupIndex with roundRobin set are the
        // candidates for that group; one is chosen per trigger (see selectZone).
        bool roundRobin = false;
        juce::String rrMode;   // "round_robin" cycles; anything else = random

        // Loop (validated against the buffer length at setMode; disabled if the
        // points fall outside the actual audio).
        bool loopEnabled = false;
        int  loopStart = 0, loopEnd = 0, loopLen = 0, loopXf = 0;
    };

    struct Voice
    {
        bool active = false;
        const SampleBuffer* buffer = nullptr;
        double position = 0.0;
        double rate = 1.0;
        int note = -1;
        int groupIndex = -1;
        float  pitchDriftDepth = 0.0f;   // random per-voice pitch-drift depth (0.4..1)
        float  volDriftDepth  = 0.0f;    // random per-voice volume-drift depth (0.4..1)
        double driftPhase = 0.0;         // independent per-voice pitch-drift LFO phase
        double volDriftPhase = 0.0;      // independent per-voice volume-drift LFO phase
        float gain = 1.0f;
        bool ampEnv = true;                // false → one-shot (no amp envelope, ignores note-off)
        CurvedAdsr::Parameters baseAdsr;   // the zone's ADSR, before runtime overrides

        bool loopEnabled = false;
        double loopEnd = 0.0, loopLen = 0.0, loopXf = 0.0;
        juce::StringArray tags;
        juce::StringArray silencedByTags;
        CurvedAdsr adsr;
        juce::uint32 startOrder = 0;

        // Short anti-click ramp, independent of the amp ADSR: fades in on note
        // start and out when choked/stolen so a mono retrigger never cuts the
        // waveform at a non-zero value.
        float declickGain = 1.0f;
        float declickDelta = 0.0f;   // >0 fading in, <0 fading out
        bool  fadingOut = false;
        bool  isRelease = false;     // release-trigger voice: plays out, ignores note-off
        bool  pedalHeld = false;     // note-off arrived while the sustain pedal was down
    };

    void renderChunk (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void handleNoteOn (int note, float velocity);
    void handleNoteOff (int note);
    void handleSustain (int cc64Value);        // sustain pedal: hold notes + fire damper-noise groups
    void handlePitchWheel (int wheelValue);   // 0..16383, centre 8192 (±2 semitones)
    Voice* allocateVoice();
    const Zone* pickZoneInGroup (int group, int note, int velocity);   // velocity layer + round-robin within one group
    void startVoice (const Zone& zone, int note, float velocity);
    CurvedAdsr::Parameters effectiveAdsr (const CurvedAdsr::Parameters& base, int groupIndex) const;

    double sampleRate = 44100.0;
    int maxBlock = 512, numChans = 2;   // for sizing per-group scratch buffers
    juce::Array<Zone> zones;
    std::vector<Voice> voices;

    // Per-group insert FX (lowpass/gain chains). Built from each group's effects in
    // setMode; null where a group has none. When any exist, voices render into
    // per-group scratch buffers, each group's chain runs, then they sum to output.
    std::vector<std::unique_ptr<FxChain>> groupChains;
    std::vector<juce::AudioBuffer<float>> groupBuffers;
    bool anyGroupFx { false };
    juce::uint32 orderCounter = 0;
    double pitchBendMul = 1.0;        // global playback-rate multiplier from the MIDI pitch wheel
    double bendRangeSemitones = 2.0;  // pitch-wheel range

    // Round-robin state, indexed by groupIndex.
    juce::Array<int> rrCounter;   // next candidate for round_robin mode
    juce::Array<int> rrLast;      // last pick for random mode (avoid repeats)
    juce::Random rrRandom;

    // Runtime amp overrides (negative = none) + per-group volume multipliers.
    float ovAttack { -1.0f }, ovDecay { -1.0f }, ovSustain { -1.0f }, ovRelease { -1.0f };
    // Envelope curve overrides. Valid curve range is [-100,+100], so a value below
    // kNoCurve means "untouched — use the mode's curve".
    static constexpr float kNoCurve = -1000.0f;
    float ovAttackCurve { kNoCurve }, ovDecayCurve { kNoCurve }, ovReleaseCurve { kNoCurve };
    juce::Array<float> groupAttack, groupDecay, groupSustain, groupRelease;   // per-group (-1 = none)
    float ovVelTrack { -1.0f };   // global velocity-tracking override (AMP_VEL_TRACK)

    // ── Modulators (LFOs) ──────────────────────────────────────────────────
    // A mode can define several: tremolo on different group sets at different rates,
    // pitch vibrato, per-group FX sweeps. Each amplitude-modulates its AMP_VOLUME
    // targets (per-sample tremolo) and control-rate-modulates GROUP_TUNING /
    // GLOBAL_TUNING / per-group FX params.
    struct Modulator
    {
        double phase { 0.0 };            // cycles, free-running
        double freqHz { 0.0 };
        float  depth { 0.0f };           // 0..1 (MOD_AMOUNT; starts at the manifest modAmount)
        int    shape { 0 };              // 0 sine · 1 triangle · 2 saw · 3 square
        juce::Array<Binding> bindings;
        juce::Array<int> ampGroups;      // groups amplitude-modulated (AMP_VOLUME + groupIndex)
        bool   ampInstrument { false };  // AMP_VOLUME with no group → modulates every voice
    };
    std::vector<Modulator> mods;
    bool hasInstMod { false };           // any modulator amplitude-modulates the whole instrument
    void applyLfoBlock (int numSamples); // advance + apply all modulators (per block)

    // Per-block modulation outputs, read in renderChunk (indexed by block position):
    std::vector<float> instTrem;                 // instrument-level amp tremolo (maxBlock)
    std::vector<std::vector<float>> groupTrem;   // per-group amp tremolo [group][maxBlock]
    juce::Array<bool>  groupHasTrem;             // groups with a group-level tremolo modulator
    double globalTuningMul { 1.0 };              // GLOBAL_TUNING vibrato (all voices)
    juce::Array<double> groupTuningModMul;       // GROUP_TUNING modulation per group (1.0 = none)
    // Global per-voice drift (all plugins): each voice gets a random depth + phase, so
    // held notes wander independently. Amounts come from the two right-side wheels.
    double driftRateHz    { 0.6 };   // pitch-drift LFO rate (Hz)
    double volDriftRateHz { 0.4 };   // volume-drift LFO rate (Hz, slightly slower)
    std::atomic<float> pitchDriftAmount  { 0.0f };   // pitch-drift wheel (0..1)
    std::atomic<float> volumeDriftAmount { 0.0f };   // volume-drift wheel (0..1)
    juce::Random driftRandom;        // per-voice depth/phase randomisation (note-on)
    bool hasDriftGateButton { false };  // library has a Drift on/off button (rides GLOBAL_TUNING)
    bool driftGateOpen      { true  };  // per-block: are the drift wheels currently engaged?
    juce::Array<float> groupVolume;       // AMP_VOLUME
    juce::Array<float> groupTagVolume;    // TAG_VOLUME (mixer knobs)
    juce::Array<float> groupGain;         // per-group output gain (linear; group-level gain effect)
    juce::Array<bool>  groupEnabled;
    juce::Array<bool>  groupReleaseTrigger;   // group trigger="release"
    juce::Array<int>   groupCcLo, groupCcHi;  // CC64-trigger range per group (-1 = not pedal-triggered)
    int  sustainValue { 0 };                  // last CC64 value (for transition detection)
    bool sustainActive { false };             // pedal currently down (holds note-offs)
    juce::Array<double> groupTuningMul;   // per-group playback-rate multiplier (GROUP_TUNING)
    float noteOnVelocity[128];            // last note-on velocity per key → release-trigger gain
};

} // namespace dm
