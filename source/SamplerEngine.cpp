#include "SamplerEngine.h"

namespace dm
{

SamplerEngine::SamplerEngine()  = default;
SamplerEngine::~SamplerEngine() = default;

juce::String SamplerEngine::getVersion()
{
    return "dehli-musikk-sampler-engine 0.2.0 (M2)";
}

void SamplerEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    currentSampleRate  = sampleRate;
    currentBlockSize   = maximumBlockSize;
    currentNumChannels = numChannels;
    voiceEngine.prepare (sampleRate, maximumBlockSize, numChannels);
    // M3+: prepare the FX chain here.
}

void SamplerEngine::setMode (const Mode& mode, const SampleSource& source)
{
    voiceEngine.setMode (mode, source);
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midi,
                                  juce::AudioPlayHead* playHead)
{
    juce::ignoreUnused (playHead);   // M6 reads transport for the auto-strum sequencer

    voiceEngine.processBlock (buffer, midi);
    // M3+: run the FX chain (lowpass + convolution) over `buffer` here.
}

void SamplerEngine::releaseResources()
{
    voiceEngine.releaseResources();
}

} // namespace dm
