#pragma once

// dehli-musikk-sampler-engine — sample source abstraction.
//
// The voice engine never touches files or blobs directly: it asks a SampleSource
// for the decoded PCM of an asset id (e.g. "flac:Bass_0C"). Backends differ in
// where the audio lives:
//   - EmbeddedFlacSource   — FLAC blobs in the binary, decoded to RAM at load.
//   - StreamingFlacSource  — streamed from disk (later milestone).
// See ../../../PLAN.md §4.

#include <juce_audio_basics/juce_audio_basics.h>

namespace dm
{

/** Decoded audio for one sample, resident in RAM. */
struct SampleBuffer
{
    juce::AudioBuffer<float> audio;   // [channels][frames]
    double sampleRate = 0.0;

    int getNumChannels() const noexcept { return audio.getNumChannels(); }
    int getNumFrames()   const noexcept { return audio.getNumSamples(); }
};

/** Resolves an asset id to its decoded PCM. Implementations own the buffers and
    must keep them alive for as long as the engine may render. */
class SampleSource
{
public:
    virtual ~SampleSource() = default;

    /** Returns the decoded buffer for an asset id, or nullptr if unknown/not decoded. */
    virtual const SampleBuffer* get (const juce::String& id) const = 0;

    /** Pin an asset for use and return its (decoded) buffer; release it when done.
        Lets a backend keep only in-use assets decoded (lazy per-mode loading, so an
        unselected mode costs no RAM). Reference-counted per id, MESSAGE THREAD only.
        Default: eager backends just return get() / no-op. */
    virtual const SampleBuffer* acquire (const juce::String& id) { return get (id); }
    virtual void release (const juce::String& id) { juce::ignoreUnused (id); }
};

} // namespace dm
