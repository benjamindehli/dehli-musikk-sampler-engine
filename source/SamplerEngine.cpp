#include "SamplerEngine.h"

namespace dm
{

SamplerEngine::SamplerEngine()  = default;
SamplerEngine::~SamplerEngine() = default;

juce::String SamplerEngine::getVersion()
{
    return "dehli-musikk-sampler-engine 0.3.0 (M3)";
}

void SamplerEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    currentSampleRate  = sampleRate;
    currentBlockSize   = maximumBlockSize;
    currentNumChannels = numChannels;
    voiceEngine.prepare (sampleRate, maximumBlockSize, numChannels);
    fxChain.prepare (sampleRate, maximumBlockSize, numChannels);
}

void SamplerEngine::setMode (const Mode& mode, const SampleSource& source)
{
    voiceEngine.setMode (mode, source);
    fxChain.setEffects (mode.effects, source);
}

void SamplerEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                  juce::MidiBuffer& midi,
                                  juce::AudioPlayHead* playHead)
{
    juce::ignoreUnused (playHead);   // M6 reads transport for the auto-strum sequencer

    voiceEngine.processBlock (buffer, midi);
    fxChain.process (buffer);
}

void SamplerEngine::releaseResources()
{
    voiceEngine.releaseResources();
    fxChain.reset();
}

} // namespace dm
