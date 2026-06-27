#include "SamplerEngine.h"

namespace dm
{

SamplerEngine::SamplerEngine()  = default;
SamplerEngine::~SamplerEngine() = default;

juce::String SamplerEngine::getVersion()
{
    return "dehli-musikk-sampler-engine 0.1.0 (M0 skeleton)";
}

void SamplerEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    currentSampleRate  = sampleRate;
    currentBlockSize   = maximumBlockSize;
    currentNumChannels = numChannels;
    // M2+: prepare voices, FX chain and sample sources here.
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midi,
                                  juce::AudioPlayHead* playHead)
{
    juce::ignoreUnused (midi, playHead);

    // M0: silence. Real voice rendering arrives in M2.
    buffer.clear();
}

void SamplerEngine::releaseResources()
{
    // M2+: free voices/FX state.
}

} // namespace dm
