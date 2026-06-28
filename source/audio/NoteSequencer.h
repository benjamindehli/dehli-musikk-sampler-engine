#pragma once

// dehli-musikk-sampler-engine — general note sequencer.
//
// Transforms incoming MIDI into the MIDI the voices should actually play: when a
// key with a SequenceTrigger is pressed, the bound NoteSequence is fired as timed
// note-on/offs (transposed, velocity-tracked, one-shot or looping). Keys without a
// trigger pass straight through, so non-sequenced modes are unaffected.
//
// "Auto-strum" is just this with one trigger per chord key. The sequencer knows
// nothing about chords or Omnichords.
//
// M6 timing: free-running — rate is in steps/second. A `Clock` abstraction
// (samples-per-step) keeps host tempo-sync a drop-in addition later.

#include <model/Manifest.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include <vector>

namespace dm
{

class NoteSequencer
{
public:
    NoteSequencer() = default;

    void prepare (double sampleRate);
    void configure (const Mode& mode);   // copies sequences + triggers
    void reset();

    /** Override the playback rate (steps/second) for all triggers; <= 0 restores
        each trigger's own rate. Thread-safe (the future StrumSpeed control). */
    void setRate (double stepsPerSecond);

    /** Added to every trigger's sequence index (clamped to the valid range) when a
        sequence starts. The runtime SEQ_INDEX — e.g. Omni-84's chord-ordering menu
        selecting a 0/84/168/252 block. Thread-safe. */
    void setIndexOffset (int offset);

    bool hasTriggers() const noexcept { return ! triggers.isEmpty(); }

    /** Produce the played MIDI for this block from the input MIDI. `out` is
        cleared first; events are emitted at sample-accurate offsets. */
    void process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples);

private:
    struct Trigger
    {
        int    sequence = 0;
        int    transpose = 0;
        double rate = 10.0;
        bool   loop = false;
        bool   trackVelocity = true;
        bool   swallow = true;
    };

    struct Fired { int note; double offStream; bool off; };

    struct Active
    {
        int    seqIndex = 0;
        int    transpose = 0;
        float  velocity = 1.0f;
        bool   loop = false;
        double samplesPerStep = 1.0;
        juce::int64 startStream = 0;
        int    nextNote = 0;
        int    triggerNote = -1;
        bool   stopping = false;
        juce::int64 stopStream = 0;
        bool   done = false;
        std::vector<Fired> fired;
    };

    void startActive (const Trigger& t, int triggerNote, float velocity, juce::int64 startStream);
    void advanceActive (Active& a, juce::MidiBuffer& out, int numSamples);

    double sampleRate = 44100.0;
    juce::Array<NoteSequence> sequences;
    juce::Array<Trigger> triggers;
    std::array<int, 128> triggerForNote { };   // index into triggers, or -1

    std::atomic<double> rateOverride { 0.0 };   // <= 0 → per-trigger rate
    std::atomic<int>    indexOffset { 0 };       // added to trigger.sequence (clamped)
    std::vector<Active> active;
    juce::int64 streamPos = 0;
};

} // namespace dm
