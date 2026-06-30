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
#include <model/Manifest.h>
#include <juce_audio_basics/juce_audio_basics.h>
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
    // override, use the mode/group value". Applied to new + active voices.
    void setAmpAttack (float seconds);
    void setAmpDecay (float seconds);
    void setAmpSustain (float level);
    void setAmpRelease (float seconds);

    /** Per-group output volume multiplier (AMP_VOLUME, e.g. Voice1/Voice2). */
    void setGroupVolume (int groupIndex, float volume);

    /** Per-group tag-addressed volume multiplier (TAG_VOLUME mixer knobs).
        Independent of setGroupVolume — both multiply, so a tag mixer and an
        AMP_VOLUME master can affect the same group without fighting. */
    void setGroupTagVolume (int groupIndex, float volume);

    /** LFO tremolo depth 0..1 (MOD_AMOUNT). 0 = no modulation. The LFO shape/rate
        and which groups it affects come from the mode's modulator (setMode). */
    void setLfoDepth (float depth) { lfoDepth = depth; }

    /** Enable/disable a group at runtime (group-level ENABLED, e.g. the Keyboard
        mode's Mono/Poly button switching between a mono and a poly group). */
    void setGroupEnabled (int groupIndex, bool enabled);

    /** Pitch-wheel bend range in semitones (default 2). Re-applied per block. */
    void setPitchBendRange (double semitones) { bendRangeSemitones = semitones; }

    /** Per-group pitch offset in semitones (GROUP_TUNING). Applied to new voices. */
    void setGroupTuning (int groupIndex, float semitones);

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
        float gain = 1.0f;
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
    CurvedAdsr::Parameters effectiveAdsr (const CurvedAdsr::Parameters& base) const;

    double sampleRate = 44100.0;
    juce::Array<Zone> zones;
    std::vector<Voice> voices;
    juce::uint32 orderCounter = 0;
    double pitchBendMul = 1.0;        // global playback-rate multiplier from the MIDI pitch wheel
    double bendRangeSemitones = 2.0;  // pitch-wheel range

    // Round-robin state, indexed by groupIndex.
    juce::Array<int> rrCounter;   // next candidate for round_robin mode
    juce::Array<int> rrLast;      // last pick for random mode (avoid repeats)
    juce::Random rrRandom;

    // Runtime amp overrides (negative = none) + per-group volume multipliers.
    float ovAttack { -1.0f }, ovDecay { -1.0f }, ovSustain { -1.0f }, ovRelease { -1.0f };
    float ovVelTrack { -1.0f };   // global velocity-tracking override (AMP_VEL_TRACK)

    // LFO tremolo (sine amp modulation on the modulator's target groups).
    double lfoPhase { 0.0 };          // cycles, free-running
    double lfoFreqHz { 0.0 };         // 0 = no LFO configured
    float  lfoDepth { 0.0f };         // 0..1 (MOD_AMOUNT)
    juce::Array<bool> lfoTargetGroup; // which groups the LFO modulates
    std::vector<float> tremBuf;       // per-sample tremolo factor scratch
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
