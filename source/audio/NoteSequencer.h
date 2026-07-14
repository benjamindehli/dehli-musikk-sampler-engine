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

    /** A ringing sequence note must change pitch (Omnichord-style chord change while
        the strum still sounds): the voice engine morphs voices on `from` to `to` —
        new sample, same elapsed position, small crossfade. */
    struct NoteMorph { int from = -1, to = -1; };

    void prepare (double sampleRate);
    void configure (const Mode& mode);   // copies sequences + triggers
    void reset();

    /** Override the playback rate (steps/second) for all triggers; <= 0 restores
        each trigger's own rate. Thread-safe (the future StrumSpeed control). */
    void setRate (double stepsPerSecond);

    /** The StrumSpeed knob's NORMALISED position (0..1). Tempo-synced mode maps it
        evenly across the note-value table (slowest at 0, fastest at 1) — the raw
        steps/s value would make the mapping depend on the current BPM. < 0 = no
        knob; the settings dropdown (setBeatsPerStep) decides. Thread-safe. */
    void setRateNorm (double norm) { rateNorm.store (norm); }

    /** Tempo-synced mode (settings menu): steps snap to the note value set by
        setBeatsPerStep at `bpm`, overriding ALL free-mode rates including the
        setRate override (a StrumSpeed knob applies that override every block, so
        it is never unset — it is the free-mode control). Sampled at strum start. */
    void setTempoSync (bool on) { tempoSync.store (on); }
    void setBpm (double bpm)    { syncBpm.store (juce::jmax (1.0, bpm)); }

    /** Step length in BEATS for tempo-synced mode (0.25 = 16th note, 1.0/6.0 =
        16th triplet, 0.375 = dotted 16th, ...). */
    void setBeatsPerStep (double beats) { beatsPerStep.store (juce::jmax (1.0e-3, beats)); }

    /** Added to every trigger's sequence index (clamped to the valid range) when a
        sequence starts. The runtime SEQ_INDEX — e.g. Omni-84's chord-ordering menu
        selecting a 0/84/168/252 block. Thread-safe. */
    void setIndexOffset (int offset);

    bool hasTriggers() const noexcept { return ! triggers.isEmpty(); }

    /** Display text for the current effective strum rate (mirrors the rate chain a
        strum start uses): tempo synced -> the snapped note value ("1/8 triplet"),
        free -> the steps/s number ("12.5"); empty when free with no rate override
        (per-trigger rates rule and there is nothing global to show). Message thread. */
    juce::String rateText() const;

    /** Produce the played MIDI for this block from the input MIDI. `out` is
        cleared first; events are emitted at sample-accurate offsets.

        `morphsOut` (optional): in select+strum mode, a chord change while strummed
        notes still ring appends the required voice morphs here (the caller forwards
        them to the voice engine before rendering). */
    void process (const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples,
                  juce::Array<NoteMorph>* morphsOut = nullptr);

private:
    double syncedBeats() const;   // tempo-synced step length in beats (knob or dropdown)
    struct Trigger
    {
        int    sequence = 0;
        int    transpose = 0;
        double rate = 10.0;
        bool   loop = false;
        bool   trackVelocity = true;
        bool   swallow = true;
    };

    struct Fired { int note; double offStream; bool off; int seqNote; };   // seqNote = index into the sequence

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
        int    orderOffset = 0;   // the sequence-list offset this strum was started with
        std::vector<Fired> fired;
    };

    void startActive (const Trigger& t, int triggerNote, float velocity, juce::int64 startStream,
                      int orderOffset, double keyRate);
    void advanceActive (Active& a, juce::MidiBuffer& out, int numSamples);
    void retargetActives (juce::Array<NoteMorph>* morphsOut);   // chord changed mid-ring

    double sampleRate = 44100.0;
    juce::Array<NoteSequence> sequences;
    juce::Array<Trigger> triggers;
    std::array<int, 128> triggerForNote { };   // index into triggers, or -1

    std::atomic<double> rateOverride { 0.0 };   // <= 0 → per-trigger rate
    std::atomic<double> rateNorm { -1.0 };       // knob position 0..1; < 0 → no knob
    std::atomic<int>    indexOffset { 0 };       // added to trigger.sequence (clamped)
    std::atomic<bool>   tempoSync { false };     // step = 1/16 note at syncBpm
    std::atomic<double> syncBpm { 120.0 };
    std::atomic<double> beatsPerStep { 0.25 };   // synced step length (0.25 = 16th)
    std::vector<Active> active;
    juce::int64 streamPos = 0;

    // ── Omnichord-style select+strum (Mode.strumKeys non-empty) ────────────────
    // sequenceTriggers become chord SELECTORS; each strum key fires the SELECTED
    // trigger's sequence + its own seqOffset (the menu indexOffset is ignored).
    // Selection changes morph ringing strums to the new chord (see NoteMorph).
    juce::Array<StrumKey> strumKeys;
    std::array<int, 128> strumForNote { };       // index into strumKeys, or -1
    bool selectMode = false;
    int  selectedTrigger = -1;                   // defaults to the first trigger

    // Released strum notes stay morphable this long (their amp-release tail may
    // still ring); older entries are certainly silent and skipping them avoids
    // morphing an unrelated newer voice on the same note number.
    static constexpr double kReleaseMorphWindowSec = 30.0;
};

} // namespace dm
