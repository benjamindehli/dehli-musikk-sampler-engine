#include "ManifestPluginProcessor.h"

namespace dm
{

ManifestPluginProcessor::ManifestPluginProcessor (Assets a)
    : juce::AudioProcessor (BusesProperties()
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      assets (std::move (a))
{
    loadEmbeddedLibrary();

    apvts = std::make_unique<juce::AudioProcessorValueTreeState> (
        *this, nullptr, "PARAMS", params::createLayout (library));
    apvts->addParameterListener (params::id::mode, this);
    apvts->addParameterListener (params::id::chordOrder, this);   // menu (e.g. amp/cabinet IR)
}

ManifestPluginProcessor::~ManifestPluginProcessor()
{
    if (apvts != nullptr)
    {
        apvts->removeParameterListener (params::id::mode, this);
        apvts->removeParameterListener (params::id::chordOrder, this);
    }
}

void ManifestPluginProcessor::loadEmbeddedLibrary()
{
    if (assets.manifestJson == nullptr || assets.manifestJsonSize <= 0 || ! assets.findResource)
        return;

    auto parsed = loadManifestFromJson (juce::String::fromUTF8 (assets.manifestJson, assets.manifestJsonSize));
    if (! parsed.ok)
    {
        DBG ("ManifestPluginProcessor: manifest load failed: " << parsed.errors.joinIntoString ("; "));
        return;
    }
    library = parsed.library;
    sampleSource = std::make_unique<EmbeddedFlacSource>();

    // Every referenced sample/IR id resolves to "<stem>.flac" in the bundle.
    juce::StringArray ids;
    for (const auto& m : library.modes)
    {
        for (const auto& g : m.groups)
            for (const auto& s : g.samples)
                ids.addIfNotAlreadyThere (s.source);
        for (const auto& e : m.effects)
            if (e.ir.isNotEmpty())
                ids.addIfNotAlreadyThere (e.ir);
        // IRs referenced only by a menu option's FX_IR_FILE binding (cabinet selector).
        for (const auto& tab : m.ui.tabs)
            for (const auto& menu : tab.menus)
                for (const auto& opt : menu.options)
                    for (const auto& b : opt.bindings)
                        if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                            ids.addIfNotAlreadyThere (b.translationValue.toString());
    }

    for (const auto& id : ids)
    {
        const auto filename = id.fromLastOccurrenceOf (":", false, false) + ".flac";
        int size = 0;
        if (const char* data = assets.findResource (filename, size))
            sampleSource->addFlac (id, data, (size_t) size);
        else
            DBG ("ManifestPluginProcessor: no embedded asset for id " << id << " (" << filename << ")");
    }

    loaded = ! library.modes.isEmpty() && sampleSource->size() > 0;
    if (loaded)
        engine.setLibrary (library, *sampleSource);
}

juce::Image ManifestPluginProcessor::loadImage (const juce::String& id)
{
    if (! assets.findResource)
        return {};

    const auto stem = id.fromLastOccurrenceOf (":", false, false);
    for (const char* ext : { ".png", ".jpg", ".jpeg" })
    {
        int size = 0;
        if (const char* data = assets.findResource (stem + ext, size))
            return juce::ImageFileFormat::loadFrom (data, (size_t) size);
    }
    return {};
}

void ManifestPluginProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
}

void ManifestPluginProcessor::releaseResources()
{
    engine.releaseResources();
}

bool ManifestPluginProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void ManifestPluginProcessor::parameterChanged (const juce::String& paramID, float)
{
    // Mode switch (rebuild) and the cabinet menu (IR reload) are both heavy +
    // message-thread work, coalesced onto the async update.
    if (paramID == params::id::mode || paramID == params::id::chordOrder)
        triggerAsyncUpdate();
}

void ManifestPluginProcessor::handleAsyncUpdate()
{
    if (apvts == nullptr)
        return;

    const int requested = (int) apvts->getRawParameterValue (params::id::mode)->load();
    const bool modeChanged = requested != engine.getActiveModeIndex();

    // Set the cabinet IR selection BEFORE (re)building the mode, so a freshly built
    // mode picks it up in buildMode (and a pure menu change applies to the live mode).
    applyMenuIrFor (requested);

    if (modeChanged)
        engine.setActiveMode (requested);

    // Only rebuild the editor UI on an actual mode change — not on a menu (IR) change.
    if (modeChanged && onModeChanged)
        onModeChanged();
}

void ManifestPluginProcessor::applyMenuIrFor (int modeIndex)
{
    if (apvts == nullptr || modeIndex < 0 || modeIndex >= library.modes.size())
        return;

    const auto& m = library.modes.getReference (modeIndex);
    if (m.ui.tabs.isEmpty())
        return;

    const auto* sel = apvts->getRawParameterValue (params::id::chordOrder);
    const int selected = sel != nullptr ? (int) sel->load() : 0;

    for (const auto& menu : m.ui.tabs.getReference (0).menus)
    {
        if (menu.options.isEmpty())
            continue;
        const int s = juce::jlimit (0, menu.options.size() - 1, selected);
        for (const auto& b : menu.options.getReference (s).bindings)
            if (b.parameter == "FX_IR_FILE" && b.effectIndex && b.translationValue.isString())
                engine.setEffectIr (*b.effectIndex, b.translationValue.toString());
        break;
    }
}

void ManifestPluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    for (auto i = getTotalNumInputChannels(); i < getTotalNumOutputChannels(); ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    keyboardState.processNextMidiBuffer (midi, 0, buffer.getNumSamples(), true);

    if (const int pw = uiPitchWheel.exchange (-1); pw >= 0)
        midi.addEvent (juce::MidiMessage::pitchWheel (1, pw), 0);
    if (const int mw = uiModWheel.exchange (-1); mw >= 0)
        midi.addEvent (juce::MidiMessage::controllerEvent (1, 1, mw), 0);

    if (loaded && apvts != nullptr)
    {
        if (auto* br = apvts->getRawParameterValue (params::id::pitchBendRange))
            engine.setPitchBendRange (br->load());
        if (const auto* m = getActiveMode())
        {
            params::applyCcToParams (midi, *m, *apvts);
            params::applyNoteSwitches (midi, *m, *apvts);
            params::applyToEngine (engine, *m, *apvts);
        }
    }

    engine.processBlock (buffer, midi, getPlayHead());

    // Output peak for the editor's level meter (max-since-read; meter resets it).
    const float blockPeak = buffer.getMagnitude (0, buffer.getNumSamples());
    if (blockPeak > outputPeak.load (std::memory_order_relaxed))
        outputPeak.store (blockPeak, std::memory_order_relaxed);
}

juce::AudioProcessorEditor* ManifestPluginProcessor::createEditor()
{
    return new ManifestPluginEditor (*this, *this);
}

void ManifestPluginProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (apvts == nullptr)
        return;
    if (auto xml = apvts->copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void ManifestPluginProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (apvts == nullptr)
        return;
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts->state.getType()))
        {
            apvts->replaceState (juce::ValueTree::fromXml (*xml));
            triggerAsyncUpdate();   // restore active mode to match the Mode param
        }
}

} // namespace dm
