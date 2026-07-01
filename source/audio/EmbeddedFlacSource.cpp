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

const SampleBuffer* EmbeddedFlacSource::get (const juce::String& id) const
{
    auto it = samples.find (id.toStdString());
    return it == samples.end() ? nullptr : it->second.pcm.get();
}

const SampleBuffer* EmbeddedFlacSource::acquire (const juce::String& id)
{
    auto it = samples.find (id.toStdString());
    if (it == samples.end())
        return nullptr;

    auto& e = it->second;
    if (e.pcm == nullptr)   // first user → decode
    {
        e.pcm = std::make_unique<SampleBuffer>();
        if (! decodeInto (*e.pcm, e.data, e.numBytes))
            e.pcm.reset();
    }
    if (! e.pinned)
        ++e.refCount;
    return e.pcm.get();
}

void EmbeddedFlacSource::release (const juce::String& id)
{
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
