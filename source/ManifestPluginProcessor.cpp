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

    // Buttons whose states swap a convolution IR (e.g. Home/Church reverb) need the
    // heavy IR reload on the message thread — listen so a click triggers the async update.
    for (const auto& m : library.modes)
        for (const auto& tab : m.ui.tabs)
            for (int i = 0; i < tab.buttons.size(); ++i)
                for (const auto& st : tab.buttons.getReference (i).states)
                    for (const auto& b : st.bindings)
                        if (b.parameter == "FX_IR_FILE")
                        {
                            const auto pid = params::buttonParamId (i);
                            if (! irButtonParams.contains (pid))
                            {
                                irButtonParams.add (pid);
                                apvts->addParameterListener (pid, this);
                            }
                        }
}

ManifestPluginProcessor::~ManifestPluginProcessor()
{
    if (apvts != nullptr)
    {
        apvts->removeParameterListener (params::id::mode, this);
        apvts->removeParameterListener (params::id::chordOrder, this);
        for (const auto& pid : irButtonParams)
            apvts->removeParameterListener (pid, this);
    }
}

void ManifestPluginProcessor::loadEmbeddedLibrary()
{
    if (! assets.findResource)
        return;

    auto loadJson = [this] (const juce::String& filename) -> juce::var
    {
        int size = 0;
        if (const char* data = assets.findResource (filename, size))
        {
            juce::var v;
            if (juce::JSON::parse (juce::String::fromUTF8 (data, size), v).wasOk())
                return v;
        }
        return {};
    };

    // Prefer the SPLIT manifest (index.json + modes/<name>.json + optional
    // partials/<name>.json), merged at load; fall back to a single embedded
    // manifest.json if no index.json is present.
    ManifestParseResult parsed;
    if (auto index = loadJson ("index.json"); ! index.isVoid())
    {
        juce::StringArray resolveErrors;
        auto merged = resolveSplitManifest (
            index,
            [&loadJson] (const juce::String& n) { return loadJson (n + ".json"); },   // modes/<n>.json
            [&loadJson] (const juce::String& n) { return loadJson (n + ".json"); },   // partials/<n>.json
            resolveErrors);
        if (! resolveErrors.isEmpty())
        {
            DBG ("ManifestPluginProcessor: split manifest resolve failed: " << resolveErrors.joinIntoString ("; "));
            return;
        }
        parsed = loadManifest (merged);
    }
    else if (assets.manifestJson != nullptr && assets.manifestJsonSize > 0)
    {
        parsed = loadManifestFromJson (juce::String::fromUTF8 (assets.manifestJson, assets.manifestJsonSize));
    }
    else
    {
        DBG ("ManifestPluginProcessor: no embedded manifest (index.json or manifest.json)");
        return;
    }

    if (! parsed.ok)
    {
        DBG ("ManifestPluginProcessor: manifest load failed: " << parsed.errors.joinIntoString ("; "));
        return;
    }
    library = parsed.library;
    sampleSource = std::make_unique<EmbeddedFlacSource>();

    // Large libraries ship their samples as a memory-mapped pack (samples.pak + .json
    // index) rather than compiled into the binary — register those first; anything the
    // pack doesn't cover (IRs, or samples for a plugin that still embeds) falls back to
    // the embedded BinaryData below.
    if (assets.packFile != juce::File() && assets.packFile.existsAsFile())
    {
        const auto indexFile = juce::File (assets.packFile.getFullPathName() + ".json");
        if (! sampleSource->openPack (assets.packFile, indexFile))
            DBG ("ManifestPluginProcessor: sample pack present but failed to open: "
                 << assets.packFile.getFullPathName());
    }

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
        // IRs referenced only by a menu option or button state's FX_IR_FILE binding
        // (cabinet/amp selector, Home/Church reverb switch).
        auto collectIr = [&ids] (const juce::Array<Binding>& bindings)
        {
            for (const auto& b : bindings)
                if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
                    ids.addIfNotAlreadyThere (b.translationValue.toString());
        };
        for (const auto& tab : m.ui.tabs)
        {
            for (const auto& menu : tab.menus)
                for (const auto& opt : menu.options)
                    collectIr (opt.bindings);
            for (const auto& btn : tab.buttons)
                for (const auto& st : btn.states)
                    collectIr (st.bindings);
        }
    }

    for (const auto& id : ids)
    {
        if (sampleSource->isRegistered (id))
            continue;   // already provided by the memory-mapped pack
        const auto filename = id.fromLastOccurrenceOf (":", false, false) + ".flac";
        int size = 0;
        if (assets.findResource != nullptr)
            if (const char* data = assets.findResource (filename, size))
            {
                sampleSource->registerFlac (id, data, (size_t) size);   // decoded lazily per active mode
                continue;
            }
        DBG ("ManifestPluginProcessor: no asset for id " << id << " (" << filename << ")");
    }

    loaded = ! library.modes.isEmpty() && sampleSource->size() > 0;
    if (loaded)
        engine.setLibrary (library, *sampleSource);
}

juce::Image ManifestPluginProcessor::loadImage (const juce::String& id)
{
    if (! assets.findResource)
        return {};

    // Cache decoded images by id — the UI asks for the same skin (e.g. one 18 MB drawbar
    // filmstrip shared by 9 drawbars, a knob strip by 30 knobs) once per control, and
    // re-decoding a big PNG each time blocked the message thread for ~17 s on the cassette
    // organ. juce::Image is ref-counted, so cached copies are cheap. Message thread only.
    const auto key = id.toStdString();
    if (auto it = imageCache.find (key); it != imageCache.end())
        return it->second;

    juce::Image image;
    const auto stem = id.fromLastOccurrenceOf (":", false, false);
    for (const char* ext : { ".png", ".jpg", ".jpeg" })
    {
        int size = 0;
        if (const char* data = assets.findResource (stem + ext, size))
        {
            image = juce::ImageFileFormat::loadFrom (data, (size_t) size);
            break;
        }
    }
    imageCache[key] = image;
    return image;
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
    // Mode switch (rebuild), the cabinet menu, and IR-swap buttons (Home/Church) are
    // all heavy + message-thread work, coalesced onto the async update.
    if (paramID == params::id::mode || paramID == params::id::chordOrder
        || irButtonParams.contains (paramID))
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
    const auto& tab = m.ui.tabs.getReference (0);

    auto applyIrBindings = [this, &m] (const juce::Array<Binding>& bindings)
    {
        for (const auto& b : bindings)
            if (b.parameter == "FX_IR_FILE" && b.translationValue.isString())
            {
                // Resolve the target effect by id (targetId), falling back to effectIndex.
                int fx = -1;
                if (b.targetId.isNotEmpty())
                    for (int i = 0; i < m.effects.size(); ++i)
                        if (m.effects.getReference (i).id == b.targetId) { fx = i; break; }
                if (fx < 0) fx = b.effectIndex.value_or (-1);
                if (fx >= 0)
                    engine.setEffectIr (fx, b.translationValue.toString());
            }
    };

    // Menu (cabinet/amp) FX_IR_FILE → selected option.
    const auto* sel = apvts->getRawParameterValue (params::id::chordOrder);
    const int selected = sel != nullptr ? (int) sel->load() : 0;
    for (const auto& menu : tab.menus)
    {
        if (menu.options.isEmpty())
            continue;
        applyIrBindings (menu.options.getReference (juce::jlimit (0, menu.options.size() - 1, selected)).bindings);
        break;
    }

    // Button (e.g. Home/Church reverb) FX_IR_FILE → selected state.
    for (int i = 0; i < tab.buttons.size(); ++i)
    {
        const auto& btn = tab.buttons.getReference (i);
        if (btn.states.isEmpty())
            continue;
        const auto* bs = apvts->getRawParameterValue (params::buttonParamId (i));
        const int s = juce::jlimit (0, btn.states.size() - 1, bs != nullptr ? (int) bs->load() : 0);
        applyIrBindings (btn.states.getReference (s).bindings);
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
        if (auto* mo = apvts->getRawParameterValue (params::id::masterOutput))
            engine.setMasterOutputGain (juce::Decibels::decibelsToGain (mo->load(), -60.0f));
        if (auto* pd = apvts->getRawParameterValue (params::id::pitchDrift))
            engine.setPitchDriftAmount (pd->load());
        if (auto* vd = apvts->getRawParameterValue (params::id::volumeDrift))
            engine.setVolumeDriftAmount (vd->load());
        if (auto* sm = apvts->getRawParameterValue (params::id::skipMuted))
            engine.setSkipMutedGroups (sm->load() > 0.5f);
        if (const auto* m = getActiveMode())
        {
            params::applyCcToParams (midi, *m, *apvts);
            params::applyNoteSwitches (midi, *m, *apvts);
            params::applyToEngine (engine, *m, *apvts, buttonClickSeq);
        }
    }

    engine.processBlock (buffer, midi, getPlayHead());

    // Per-channel output peak for the editor's stereo level meter (max-since-read; the
    // meter resets it). Mono output feeds both bars the same.
    const int n = buffer.getNumSamples();
    const float peakL = buffer.getMagnitude (0, 0, n);
    const float peakR = buffer.getNumChannels() > 1 ? buffer.getMagnitude (1, 0, n) : peakL;
    if (peakL > outputPeakL.load (std::memory_order_relaxed)) outputPeakL.store (peakL, std::memory_order_relaxed);
    if (peakR > outputPeakR.load (std::memory_order_relaxed)) outputPeakR.store (peakR, std::memory_order_relaxed);
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
