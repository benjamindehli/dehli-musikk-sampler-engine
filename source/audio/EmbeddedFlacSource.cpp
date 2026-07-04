#include "EmbeddedFlacSource.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace dm
{

EmbeddedFlacSource::EmbeddedFlacSource()
{
    formats.registerBasicFormats(); // includes FLAC when JUCE_USE_FLAC=1
}

bool EmbeddedFlacSource::decodeInto (SampleBuffer& out, const void* data, size_t numBytes)
{
    // MemoryInputStream does not copy; the reader is consumed synchronously here.
    auto stream = std::make_unique<juce::MemoryInputStream> (data, numBytes, false);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (std::move (stream)));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    out.sampleRate = reader->sampleRate;
    out.audio.setSize ((int) juce::jmax (1u, reader->numChannels), (int) reader->lengthInSamples);
    reader->read (&out.audio, 0, (int) reader->lengthInSamples, 0, true, true);
    return true;
}

bool EmbeddedFlacSource::addFlac (const juce::String& id, const void* data, size_t numBytes)
{
    auto pcm = std::make_unique<SampleBuffer>();
    if (! decodeInto (*pcm, data, numBytes))
        return false;

    Entry e;
    e.data = data; e.numBytes = numBytes; e.pcm = std::move (pcm); e.pinned = true;
    samples[id.toStdString()] = std::move (e);
    return true;
}

void EmbeddedFlacSource::registerFlac (const juce::String& id, const void* data, size_t numBytes)
{
    Entry e;
    e.data = data; e.numBytes = numBytes;   // decoded lazily in acquire()
    samples[id.toStdString()] = std::move (e);
}

bool EmbeddedFlacSource::openPack (const juce::File& dataFile, const juce::File& indexFile)
{
    if (! dataFile.existsAsFile() || ! indexFile.existsAsFile())
        return false;

    pack = std::make_unique<juce::MemoryMappedFile> (dataFile, juce::MemoryMappedFile::readOnly);
    const auto* base = static_cast<const char*> (pack->getData());
    const auto  mapSize = (juce::int64) pack->getSize();
    if (base == nullptr || mapSize <= 0)
    {
        pack.reset();
        return false;
    }

    juce::var index;
    if (juce::JSON::parse (indexFile.loadFileAsString(), index).failed() || ! index.isArray())
    {
        pack.reset();
        return false;
    }

    // Index is an array of { id, o(ffset), l(ength) } — register each as a lazy slice.
    for (const auto& entry : *index.getArray())
    {
        if (auto* o = entry.getDynamicObject())
        {
            const auto id     = o->getProperty ("id").toString();
            const auto offset = (juce::int64) o->getProperty ("o");
            const auto length = (juce::int64) o->getProperty ("l");
            if (id.isNotEmpty() && offset >= 0 && length > 0 && offset + length <= mapSize)
                registerFlac (id, base + offset, (size_t) length);
        }
    }
    return true;
}

bool EmbeddedFlacSource::isRegistered (const juce::String& id) const
{
    return samples.find (id.toStdString()) != samples.end();
}

const SampleBuffer* EmbeddedFlacSource::get (const juce::String& id) const
{
    auto it = samples.find (id.toStdString());
    return it == samples.end() ? nullptr : it->second.pcm.get();
}

const SampleBuffer* EmbeddedFlacSource::acquire (const juce::String& id)
{
    const auto key = id.toStdString();

    // Phase 1 (locked, quick): if already decoded, just pin it; else grab the compressed
    // bytes to decode. We do NOT decode under the lock — that would serialise the parallel
    // build workers (decode is the slow part). The map structure is stable during a build
    // (all entries registered up front), so concurrent finds are safe.
    const void* data = nullptr;
    size_t numBytes = 0;
    {
        const std::lock_guard<std::mutex> lock (mutex);
        auto it = samples.find (key);
        if (it == samples.end())
            return nullptr;
        auto& e = it->second;
        if (e.pcm != nullptr)
        {
            if (! e.pinned) ++e.refCount;
            return e.pcm.get();
        }
        data = e.data; numBytes = e.numBytes;
    }

    // Phase 2 (unlocked, slow): decode. Distinct ids decode concurrently; two threads on
    // the same id both decode and the loser's buffer is discarded below (harmless — the
    // build hands each worker distinct ids, so that doesn't happen in practice).
    auto decoded = std::make_unique<SampleBuffer>();
    if (! decodeInto (*decoded, data, numBytes))
        decoded.reset();

    // Phase 3 (locked, quick): publish + pin.
    {
        const std::lock_guard<std::mutex> lock (mutex);
        auto it = samples.find (key);
        if (it == samples.end())
            return nullptr;
        auto& e = it->second;
        if (e.pcm == nullptr)
            e.pcm = std::move (decoded);
        if (! e.pinned) ++e.refCount;
        return e.pcm.get();
    }
}

void EmbeddedFlacSource::release (const juce::String& id)
{
    const std::lock_guard<std::mutex> lock (mutex);
    auto it = samples.find (id.toStdString());
    if (it == samples.end())
        return;

    auto& e = it->second;
    if (e.pinned || e.refCount <= 0)
        return;
    if (--e.refCount == 0)
        e.pcm.reset();   // free the decoded PCM (keeps the compressed ref for re-acquire)
}

} // namespace dm
