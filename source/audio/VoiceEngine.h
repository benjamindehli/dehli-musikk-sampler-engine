#pragma once

// dehli-musikk-sampler-engine — polyphonic sample voice engine.
//
// M2 scope (the Bass mode): one dedicated sample per note, fixed-pitch playback
// resampled to the device rate, a per-voice amp ADSR resolved from the mode/group
// envelope, and monophonic tag-choke. Velocity layers, round-robin, loops,
// triggers, sequences and LFOs are represented in the model but not yet
// interpreted here — those arrive in later milestones.

#include "SampleSource.h"
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

private:
    struct Zone
    {
        int loNote = 0, hiNote = 127, rootNote = 60;
        const SampleBuffer* buffer = nullptr;
        bool pitchKeyTrack = false;
        juce::ADSR::Parameters adsr;
        float gain = 1.0f;
        float velTrack = 0.0f;
        int groupIndex = -1;
        juce::StringArray tags;
        juce::StringArray silencedByTags;
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
        juce::StringArray tags;
        juce::StringArray silencedByTags;
        juce::ADSR adsr;
        juce::uint32 startOrder = 0;

        // Short anti-click ramp, independent of the amp ADSR: fades in on note
        // start and out when choked/stolen so a mono retrigger never cuts the
        // waveform at a non-zero value.
        float declickGain = 1.0f;
        float declickDelta = 0.0f;   // >0 fading in, <0 fading out
        bool  fadingOut = false;
    };

    void renderChunk (juce::AudioBuffer<float>& buffer, int startSample, int numSamples);
    void handleNoteOn (int note, float velocity);
    void handleNoteOff (int note);
    Voice* allocateVoice();
    const Zone* findZone (int note) const;

    double sampleRate = 44100.0;
    juce::Array<Zone> zones;
    std::vector<Voice> voices;
    juce::uint32 orderCounter = 0;
};

} // namespace dm
