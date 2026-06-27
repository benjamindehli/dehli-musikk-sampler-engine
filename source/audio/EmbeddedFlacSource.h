#pragma once

// dehli-musikk-sampler-engine — embedded FLAC sample backend.
//
// Decodes FLAC blobs (from juce_add_binary_data in the plugin, or from memory in
// tests) into PCM resident in RAM. Zero added playback latency; used by Omni-84.

#include "SampleSource.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <string>
#include <unordered_map>

namespace dm
{

class EmbeddedFlacSource : public SampleSource
{
public:
    EmbeddedFlacSource();

    /** Decode a FLAC blob and store it under `id`. Returns false if the blob
        could not be decoded (the id is then left unset). */
    bool addFlac (const juce::String& id, const void* data, size_t numBytes);

    const SampleBuffer* get (const juce::String& id) const override;

    int size() const noexcept { return (int) samples.size(); }

private:
    juce::AudioFormatManager formats;
    std::unordered_map<std::string, std::unique_ptr<SampleBuffer>> samples;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmbeddedFlacSource)
};

} // namespace dm
