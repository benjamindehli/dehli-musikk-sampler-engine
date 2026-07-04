#pragma once

// dehli-musikk-sampler-engine — embedded FLAC sample backend.
//
// Decodes FLAC blobs (from juce_add_binary_data in the plugin, or from memory in
// tests) into PCM resident in RAM. Zero added playback latency; used by Omni-84.

#include "SampleSource.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dm
{

class EmbeddedFlacSource : public SampleSource
{
public:
    EmbeddedFlacSource();

    /** Decode a FLAC blob NOW and pin it under `id` (never freed). Returns false if
        it couldn't be decoded. Eager — used by tests / direct use. */
    bool addFlac (const juce::String& id, const void* data, size_t numBytes);

    /** Register a FLAC blob WITHOUT decoding — decoded lazily on acquire(), freed on
        the last release(). `data` must stay valid for the source's lifetime (it's the
        plugin's embedded BinaryData). This is the low-RAM per-mode path. */
    void registerFlac (const juce::String& id, const void* data, size_t numBytes);

    /** Memory-map a sample pack file (concatenated FLAC blobs) and register each id
        from its JSON index ({"flac:x": [offset, length], …}) as a slice of the mapping.
        The OS pages in only the slices actually decoded, so a multi-GB pack costs no
        launch time or resident RAM — the alternative to compiling samples into the
        binary. Returns false if the file or index can't be opened. */
    bool openPack (const juce::File& dataFile, const juce::File& indexFile);

    /** True if an id has been registered (embedded or packed), decoded or not. */
    bool isRegistered (const juce::String& id) const;

    const SampleBuffer* get (const juce::String& id) const override;
    const SampleBuffer* acquire (const juce::String& id) override;
    void release (const juce::String& id) override;

    int size() const noexcept { return (int) samples.size(); }

private:
    struct Entry
    {
        const void* data = nullptr;   // compressed FLAC bytes (embedded; not owned)
        size_t numBytes = 0;
        std::unique_ptr<SampleBuffer> pcm;   // decoded; null until acquired
        int  refCount = 0;
        bool pinned = false;          // addFlac() entries: always decoded, never freed
    };

    bool decodeInto (SampleBuffer& out, const void* data, size_t numBytes);

    juce::AudioFormatManager formats;
    std::unordered_map<std::string, Entry> samples;
    std::unique_ptr<juce::MemoryMappedFile> pack;   // backing store for packed sample slices
    // acquire()/release() may now run on a background load thread while the message
    // thread retires an old mode — serialise them. get() (audio thread) stays lock-free:
    // it only reads the CURRENT mode's already-decoded, ref-held entries, which are never
    // the entry a concurrent acquire is decoding or a release is freeing.
    std::mutex mutex;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmbeddedFlacSource)
};

} // namespace dm
