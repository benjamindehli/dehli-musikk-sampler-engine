#include "EmbeddedFlacSource.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace dm
{

EmbeddedFlacSource::EmbeddedFlacSource()
{
    formats.registerBasicFormats(); // includes FLAC when JUCE_USE_FLAC=1
}

bool EmbeddedFlacSource::addFlac (const juce::String& id, const void* data, size_t numBytes)
{
    // MemoryInputStream does not copy; the reader is consumed synchronously below,
    // so the caller's blob only needs to outlive this call.
    auto stream = std::make_unique<juce::MemoryInputStream> (data, numBytes, false);

    std::unique_ptr<juce::AudioFormatReader> reader (
        formats.createReaderFor (std::move (stream)));

    if (reader == nullptr || reader->lengthInSamples <= 0)
        return false;

    auto buffer = std::make_unique<SampleBuffer>();
    buffer->sampleRate = reader->sampleRate;
    buffer->audio.setSize ((int) juce::jmax (1u, reader->numChannels),
                           (int) reader->lengthInSamples);

    reader->read (&buffer->audio, 0, (int) reader->lengthInSamples, 0, true, true);

    samples[id.toStdString()] = std::move (buffer);
    return true;
}

const SampleBuffer* EmbeddedFlacSource::get (const juce::String& id) const
{
    auto it = samples.find (id.toStdString());
    return it == samples.end() ? nullptr : it->second.get();
}

} // namespace dm
