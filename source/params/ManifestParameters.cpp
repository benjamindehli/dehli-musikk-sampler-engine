#include "ManifestParameters.h"
#include <set>

namespace dm::params
{

// ---------------------------------------------------------------------------
// Registry of engine-SUPPORTED float controls. createLayout discovers which of
// these a library actually uses (and, for perGroup, which group indices), so the
// param set is manifest-driven. Order here = order of the DAW automation lanes.
// Adding a new engine float parameter = add a row here + make the engine apply it.
// ---------------------------------------------------------------------------
struct FloatSpec
{
    const char* engineParam;   // binding.parameter to match
    const char* baseId;        // stable param id (perGroup → baseId + (group+1))
    const char* baseName;      // DAW display name
    bool        perGroup;      // one param per distinct group index (e.g. Voice 1/2)
};

static const FloatSpec kFloatSpecs[] =
{
    { "FX_FILTER_FREQUENCY", "filter",     "Filter",      false },
    { "FX_MIX",              "reverbMix",  "Reverb Mix",  false },
    { "ENV_RELEASE",         "release",    "Release",     false },
    { "ENV_DECAY",           "decay",      "Decay",       false },
    { "ENV_ATTACK",          "attack",     "Attack",      false },
    { "ENV_SUSTAIN",         "sustain",    "Sustain",     false },
    { "FX_OUTPUT_LEVEL",     "reverbGain", "Reverb Gain", false },
    { "AMP_VOLUME",          "voice",      "Voice",       true  },
    { "SEQ_PLAYBACK_RATE",   "strumSpeed", "Strum Speed", false },
    { "GROUP_TUNING",        "tuning",     "Tuning",      true  },   // per-group pitch (semitones)
    { "FX_DRIVE",            "drive",      "Drive",       false },   // wave_shaper
    { "AMP_VEL_TRACK",       "velSens",    "Velocity Sensitivity", false },
};

// Bool buttons, classified by their state bindings. Order = lane order.
struct BoolSpec { const char* id; const char* name; };
static const BoolSpec kBoolSpecs[] =
{
    { "fxEnable", "FX Enable" },       // 0: ENABLED on an effect
    { "monoPoly", "Mono / Poly" },     // 1: ENABLED on a group
    { "velTrack", "Velocity Track" },  // 2: AMP_VEL_TRACK
};

static const FloatSpec* floatSpecFor (const juce::String& parameter)
{
    for (const auto& s : kFloatSpecs)
        if (parameter == s.engineParam)
            return &s;
    return nullptr;
}

static juce::String floatId (const FloatSpec& s, std::optional<int> groupIndex)
{
    if (s.perGroup)
        return juce::String (s.baseId) + juce::String (groupIndex.value_or (0) + 1);
    return s.baseId;
}

// Index into kBoolSpecs for a button, or -1.
static int buttonKindIndex (const Button& b)
{
    for (const auto& st : b.states)
        for (const auto& bd : st.bindings)
        {
            if (bd.parameter == "ENABLED" && bd.effectIndex)      return 0; // fxEnable
            if (bd.parameter == "ENABLED" && bd.level == "group") return 1; // monoPoly
            if (bd.parameter == "AMP_VEL_TRACK")                  return 2; // velTrack
        }
    return -1;
}

static double normOf (const Control& c)
{
    const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);
    const double v  = c.value.value_or (mn);
    return mx > mn ? juce::jlimit (0.0, 1.0, (v - mn) / (mx - mn)) : 0.0;
}

static bool controlHasFloatSpec (const Control& c)
{
    for (const auto& b : c.bindings)
        if (floatSpecFor (b.parameter) != nullptr)
            return true;
    return false;
}

// Tag/selector controls (TAG_VOLUME mixers, TAG_ENABLED selectors) get one param
// per knob, keyed by a stable id from the control's label.
static bool controlIsTag (const Control& c)
{
    if (controlHasFloatSpec (c))
        return false;
    for (const auto& b : c.bindings)
        if (b.parameter == "TAG_VOLUME" || b.parameter == "TAG_ENABLED")
            return true;
    return false;
}

static juce::String controlKey (const juce::String& label)
{
    return "ctl_" + label.toLowerCase().replaceCharacters (" :-/.,()#&", "__________");
}

// Map a normalised knob value to the engine value: a binding's explicit output
// range (DecentSampler translationOutputMin/Max) if present, else the control's
// own min..max range times the factor.
static double outputValue (const Binding& b, float norm, double mn, double mx)
{
    if (b.translationOutputMin && b.translationOutputMax)
        return *b.translationOutputMin + (double) norm * (*b.translationOutputMax - *b.translationOutputMin);
    return (mn + (double) norm * (mx - mn)) * b.factor.value_or (1.0);
}

// "in,out;in,out;..." lookup — returns the output whose input is closest to `x`
// (DecentSampler step/radio tables for TAG_ENABLED/VISIBLE/OPACITY selectors).
static double evalTable (const juce::String& table, double x)
{
    double bestOut = 0.0, bestDist = 1.0e18;
    int start = 0;
    while (start < table.length())
    {
        int semi = table.indexOfChar (start, ';');
        if (semi < 0) semi = table.length();
        const int comma = table.indexOfChar (start, ',');
        if (comma > start && comma < semi)
        {
            const double in  = table.substring (start, comma).getDoubleValue();
            const double out = table.substring (comma + 1, semi).getDoubleValue();
            const double d = in > x ? in - x : x - in;
            if (d < bestDist) { bestDist = d; bestOut = out; }
        }
        start = semi + 1;
    }
    return bestOut;
}

// ---------------------------------------------------------------------------
// binding -> engine vocabulary. `value` is the final engine value (native units,
// factor already applied).
// ---------------------------------------------------------------------------
static void applyBinding (SamplerEngine& eng, const Binding& b, double value)
{
    const auto& p = b.parameter;
    if      (p == "FX_FILTER_FREQUENCY") eng.setLowpassFrequency ((float) value);
    else if (p == "FX_MIX")              eng.setReverbMix ((float) value);
    else if (p == "FX_OUTPUT_LEVEL")     eng.setReverbWetGainDb ((float) value);
    else if (p == "SEQ_PLAYBACK_RATE")   eng.setSequencerRate (value);
    else if (p == "ENV_ATTACK")          eng.setAmpAttack ((float) value);
    else if (p == "ENV_DECAY")           eng.setAmpDecay ((float) value);
    else if (p == "ENV_SUSTAIN")         eng.setAmpSustain ((float) value);
    else if (p == "ENV_RELEASE")         eng.setAmpRelease ((float) value);
    else if (p == "AMP_VOLUME")          eng.setGroupVolume (b.groupIndex.value_or (0), (float) value);
    else if (p == "GROUP_TUNING")        eng.setGroupTuning (b.groupIndex.value_or (0), (float) value);
    else if (p == "AMP_VEL_TRACK")       eng.setAmpVelTrack ((float) value);
    else if (p == "ENABLED")
    {
        const bool on = value > 0.5;
        if (b.level == "group" && b.groupIndex) eng.setGroupEnabled (*b.groupIndex, on);
        else if (b.effectIndex)                 eng.setEffectEnabled (*b.effectIndex, on);
    }
    // AMP_VEL_TRACK etc.: no engine runtime param yet.
}

static bool bindingIsTrue (const Binding& b)
{
    return b.translationValue.isBool() ? (bool) b.translationValue
                                       : b.translationValue.toString().equalsIgnoreCase ("true");
}

static void applyButtonState (SamplerEngine& eng, const ButtonState& st)
{
    for (const auto& b : st.bindings)
    {
        if (b.parameter == "PATH") continue;
        if (b.parameter == "ENABLED")
        {
            const bool on = bindingIsTrue (b);
            if (b.level == "group" && b.groupIndex) eng.setGroupEnabled (*b.groupIndex, on);
            else if (b.effectIndex)                 eng.setEffectEnabled (*b.effectIndex, on);
        }
    }
}

// ---------------------------------------------------------------------------

static juce::StringArray firstMenuOptions (const PresetLibrary& lib)
{
    juce::StringArray names;
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
            for (const auto& menu : m.ui.tabs.getReference (0).menus)
                if (! menu.options.isEmpty())
                {
                    for (const auto& o : menu.options) names.add (o.name);
                    return names;
                }
    return names;
}

// First control across all modes driving (spec, groupIndex) → its default norm.
static float defaultNorm (const PresetLibrary& lib, const FloatSpec& spec, std::optional<int> groupIndex)
{
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
            for (const auto& c : m.ui.tabs.getReference (0).controls)
                for (const auto& b : c.bindings)
                    if (b.parameter == spec.engineParam
                        && (! spec.perGroup || b.groupIndex.value_or (0) == groupIndex.value_or (0)))
                        return (float) normOf (c);
    return 0.5f;
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const PresetLibrary& lib)
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray modeNames;
    for (const auto& m : lib.modes) modeNames.add (m.name);
    if (modeNames.isEmpty()) modeNames.add ("Default");
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::mode, 1 }, "Mode", modeNames, 0));

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id::pitchBendRange, 1 }, "Pitch Bend Range", 1, 12, 2));

    // Float params: registry order; create only targets the library actually uses.
    auto libraryUses = [&lib] (const FloatSpec& s, std::optional<int> gi) -> bool
    {
        for (const auto& m : lib.modes)
            if (! m.ui.tabs.isEmpty())
                for (const auto& c : m.ui.tabs.getReference (0).controls)
                    for (const auto& b : c.bindings)
                        if (b.parameter == s.engineParam
                            && (! s.perGroup || ! gi || b.groupIndex.value_or (0) == *gi))
                            return true;
        return false;
    };

    for (const auto& s : kFloatSpecs)
    {
        if (s.perGroup)
        {
            std::set<int> groups;
            for (const auto& m : lib.modes)
                if (! m.ui.tabs.isEmpty())
                    for (const auto& c : m.ui.tabs.getReference (0).controls)
                        for (const auto& b : c.bindings)
                            if (b.parameter == s.engineParam)
                                groups.insert (b.groupIndex.value_or (0));
            for (int gi : groups)
                p.push_back (std::make_unique<AudioParameterFloat> (
                    ParameterID { floatId (s, gi), 1 }, s.baseName + (" " + String (gi + 1)),
                    NormalisableRange<float> (0.0f, 1.0f), defaultNorm (lib, s, gi)));
        }
        else if (libraryUses (s, {}))
        {
            p.push_back (std::make_unique<AudioParameterFloat> (
                ParameterID { s.baseId, 1 }, s.baseName,
                NormalisableRange<float> (0.0f, 1.0f), defaultNorm (lib, s, {})));
        }
    }

    // Bool params: registry order; create only kinds present in the library.
    constexpr int numBoolSpecs = (int) (sizeof (kBoolSpecs) / sizeof (kBoolSpecs[0]));
    for (int k = 0; k < numBoolSpecs; ++k)
    {
        bool present = false;
        for (const auto& m : lib.modes)
            if (! m.ui.tabs.isEmpty())
                for (const auto& b : m.ui.tabs.getReference (0).buttons)
                    if (buttonKindIndex (b) == k) { present = true; break; }
        if (present)
            p.push_back (std::make_unique<AudioParameterBool> (
                ParameterID { kBoolSpecs[k].id, 1 }, kBoolSpecs[k].name, false));
    }

    // Chord-order menu (first dropdown found).
    auto chordOpts = firstMenuOptions (lib);
    if (! chordOpts.isEmpty())
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::chordOrder, 1 }, "Chord Order", chordOpts, 0));

    // Tag/selector controls — one float param per knob (TAG_VOLUME mixers, TAG_ENABLED selectors).
    std::set<juce::String> seenTag;
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
            for (const auto& c : m.ui.tabs.getReference (0).controls)
                if (controlIsTag (c))
                {
                    const auto cid = controlKey (c.label);
                    if (seenTag.insert (cid).second)
                        p.push_back (std::make_unique<AudioParameterFloat> (
                            ParameterID { cid, 1 }, c.label, NormalisableRange<float> (0.0f, 1.0f), (float) normOf (c)));
                }

    return { p.begin(), p.end() };
}

// ---------------------------------------------------------------------------

void applyToEngine (SamplerEngine& engine, const Mode& mode,
                    juce::AudioProcessorValueTreeState& apvts)
{
    if (mode.ui.tabs.isEmpty())
        return;
    const auto& tab = mode.ui.tabs.getReference (0);

    auto raw = [&apvts] (juce::StringRef pid) -> float
    {
        if (auto* a = apvts.getRawParameterValue (pid)) return a->load();
        return 0.0f;
    };

    for (const auto& c : tab.controls)
    {
        const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);

        // Float-spec control: each matching binding drives its engine target.
        if (controlHasFloatSpec (c))
        {
            for (const auto& b : c.bindings)
            {
                const auto* spec = floatSpecFor (b.parameter);
                if (spec == nullptr)
                    continue;
                const float norm = spec->perGroup ? raw (floatId (*spec, b.groupIndex))
                                                  : raw (juce::StringRef (spec->baseId));
                applyBinding (engine, b, outputValue (b, norm, mn, mx));
            }
            continue;
        }

        // Tag/selector control: one knob param drives its tag bindings.
        if (controlIsTag (c))
        {
            const float norm = raw (controlKey (c.label));
            for (const auto& b : c.bindings)
            {
                if (b.parameter == "TAG_VOLUME")
                {
                    const float v = (float) outputValue (b, norm, mn, mx);
                    for (int gi = 0; gi < mode.groups.size(); ++gi)
                        if (mode.groups.getReference (gi).tags.contains (b.identifier))
                            engine.setGroupVolume (gi, v);
                }
                else if (b.parameter == "TAG_ENABLED")
                {
                    const double sel = mn + (double) norm * (mx - mn);   // selector value in control range
                    const bool on = evalTable (b.translationTable, sel) > 0.5;
                    for (int gi = 0; gi < mode.groups.size(); ++gi)
                        if (mode.groups.getReference (gi).tags.contains (b.identifier))
                            engine.setGroupEnabled (gi, on);
                }
            }
        }
    }

    for (const auto& btn : tab.buttons)
    {
        const int k = buttonKindIndex (btn);
        if (k < 0) continue;
        const int stateIndex = raw (kBoolSpecs[k].id) > 0.5f ? 1 : 0;
        if (stateIndex >= 0 && stateIndex < btn.states.size())
            applyButtonState (engine, btn.states.getReference (stateIndex));
    }

    for (const auto& menu : tab.menus)
        if (! menu.options.isEmpty())
        {
            const int sel = juce::jlimit (0, menu.options.size() - 1, (int) raw (id::chordOrder));
            engine.setSequencerIndexOffset (menu.options.getReference (sel).seqIndex);
            break;
        }
}

void applyCcToParams (const juce::MidiBuffer& midi, const Mode& mode,
                      juce::AudioProcessorValueTreeState& apvts)
{
    if (mode.ccBindings.isEmpty())
        return;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isController())
            continue;

        const int   cc = msg.getControllerNumber();
        const float v  = msg.getControllerValue() / 127.0f;

        for (const auto& cb : mode.ccBindings)
        {
            if (cb.cc != cc)
                continue;
            const auto* spec = floatSpecFor (cb.parameter);
            if (spec == nullptr)
                continue;
            const auto pid = floatId (*spec, cb.groupIndex);
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (cb.normMin + v * (cb.normMax - cb.normMin)));
            if (auto* prm = apvts.getParameter (pid))
                prm->setValueNotifyingHost (norm);
        }
    }
}

void applyNoteSwitches (const juce::MidiBuffer& midi, const Mode& mode,
                        juce::AudioProcessorValueTreeState& apvts)
{
    if (mode.menuKeySwitches.isEmpty())
        return;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isNoteOn())
            continue;
        const int note = msg.getNoteNumber();
        for (const auto& ks : mode.menuKeySwitches)
            if (ks.note == note)
                if (auto* prm = apvts.getParameter (id::chordOrder))
                    prm->setValueNotifyingHost (prm->convertTo0to1 ((float) ks.option));
    }
}

juce::StringArray controlParamIds (const Control& c)
{
    juce::StringArray ids;
    for (const auto& b : c.bindings)
        if (const auto* spec = floatSpecFor (b.parameter))
            ids.addIfNotAlreadyThere (floatId (*spec, b.groupIndex));
    if (ids.isEmpty() && controlIsTag (c))
        ids.add (controlKey (c.label));   // TAG_VOLUME / TAG_ENABLED knob → its own param
    return ids;
}

juce::String buttonParamId (const Button& b)
{
    const int k = buttonKindIndex (b);
    return k >= 0 ? juce::String (kBoolSpecs[k].id) : juce::String();
}

} // namespace dm::params
