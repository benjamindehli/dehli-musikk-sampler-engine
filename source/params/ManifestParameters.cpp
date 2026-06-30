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
    { "FX_OUTPUT_LEVEL",     "fxOutput",   "Output Level", false },   // convolution wet trim OR wave_shaper output
    { "AMP_VOLUME",          "voice",      "Voice",       true  },
    { "SEQ_PLAYBACK_RATE",   "strumSpeed", "Strum Speed", false },
    { "GROUP_TUNING",        "tuning",     "Tuning",      true  },   // per-group pitch (semitones)
    { "FX_DRIVE",            "drive",      "Drive",       false },   // wave_shaper
    { "AMP_VEL_TRACK",       "velSens",    "Velocity Sensitivity", false },
    { "LEVEL",               "level",      "Level",       false },   // gain effect
    { "MOD_AMOUNT",          "modAmount",  "Mod Amount",  false },   // LFO depth (engine wiring: feature #3)
};

// With per-control keying the baseId/perGroup/baseName fields are vestigial — each
// control now owns one param keyed by its label (see controlKey). The registry is
// kept only as the set of float engine parameters applyBinding knows how to route.

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

static bool bindingIsTag (const Binding& b)
{
    return b.parameter == "TAG_VOLUME" || b.parameter == "TAG_ENABLED";
}

// A control "drives the engine" if it has any binding the engine acts on — a float
// engine parameter (registry) or a tag mixer/selector. Such controls each get ONE
// automatable param, keyed by the control's label (controlKey). UI-only controls
// (e.g. VISIBLE/OPACITY-only) get none.
static bool controlDrivesEngine (const Control& c)
{
    for (const auto& b : c.bindings)
        if (floatSpecFor (b.parameter) != nullptr || bindingIsTag (b))
            return true;
    return false;
}

// Stable param id from a control's label. Controls with the same label across modes
// share one param (a knob's lane stays put when you switch modes).
static juce::String controlKey (const juce::String& label)
{
    return "ctl_" + label.toLowerCase().replaceCharacters (" :-/.,()#&", "__________");
}

// Map a normalised knob value to the engine value: a binding's explicit output
// range (DecentSampler translationOutputMin/Max) if present, else the control's
// own min..max range times the factor.
// "in,out;in,out;..." lookup — piecewise-linear interpolation between the points
// (DecentSampler curve tables, e.g. a Saturation knob's FX_DRIVE / FX_OUTPUT_LEVEL).
// Clamps to the first/last output outside the table's input range.
static double evalTableLinear (const juce::String& table, double x)
{
    double prevIn = 0.0, prevOut = 0.0; bool havePrev = false;
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
            if (x <= in)
            {
                if (! havePrev || ! (in > prevIn)) return out;   // before first point / vertical step
                const double t = (x - prevIn) / (in - prevIn);
                return prevOut + t * (out - prevOut);
            }
            prevIn = in; prevOut = out; havePrev = true;
        }
        start = semi + 1;
    }
    return havePrev ? prevOut : 0.0;   // past the last point → last output
}

static double outputValue (const Binding& b, float norm, double mn, double mx)
{
    if (b.translationTable.isNotEmpty())
    {
        double in = mn + (double) norm * (mx - mn);          // the control's actual value
        if (b.translationReversed) in = mx + mn - in;        // reverse the input (max↔min)
        return evalTableLinear (b.translationTable, in);
    }
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
    const bool  groupLevel = b.level == "group" && b.groupIndex.has_value();

    if      (p == "FX_FILTER_FREQUENCY") eng.setLowpassFrequency ((float) value);
    // Effect params: address a specific instrument effect by index when known;
    // FxChain routes FX_OUTPUT_LEVEL by the slot's kind (wave_shaper vs convolution).
    else if (p == "FX_MIX")              { if (b.effectIndex) eng.setEffectParam (*b.effectIndex, p, (float) value); else eng.setReverbMix ((float) value); }
    else if (p == "FX_DRIVE")            { if (b.effectIndex) eng.setEffectParam (*b.effectIndex, p, (float) value); else eng.setWaveShaperDrive ((float) value); }
    else if (p == "FX_OUTPUT_LEVEL")     { if (b.effectIndex) eng.setEffectParam (*b.effectIndex, p, (float) value); else eng.setReverbWetGainDb ((float) value); }
    else if (p == "LEVEL")
    {
        // group-level `gain` effect → per-group output gain; instrument gain → by index.
        if      (groupLevel)    eng.setGroupGain (*b.groupIndex, (float) value);
        else if (b.effectIndex) eng.setEffectParam (*b.effectIndex, p, (float) value);
        else                    eng.setGain ((float) value);
    }
    else if (p == "SEQ_PLAYBACK_RATE")   eng.setSequencerRate (value);
    else if (p == "ENV_ATTACK")          eng.setAmpAttack ((float) value);
    else if (p == "ENV_DECAY")           eng.setAmpDecay ((float) value);
    else if (p == "ENV_SUSTAIN")         eng.setAmpSustain ((float) value);
    else if (p == "ENV_RELEASE")         eng.setAmpRelease ((float) value);
    else if (p == "AMP_VOLUME")          { if (b.groupIndex) eng.setGroupVolume (*b.groupIndex, (float) value); else eng.setMasterVolume ((float) value); }
    else if (p == "GROUP_TUNING")        eng.setGroupTuning (b.groupIndex.value_or (0), (float) value);
    else if (p == "AMP_VEL_TRACK")       eng.setAmpVelTrack ((float) value);
    else if (p == "MOD_AMOUNT")          eng.setLfoDepth ((float) value);
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

juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const PresetLibrary& lib)
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    StringArray modeNames;
    for (const auto& m : lib.modes) modeNames.add (m.name);
    if (modeNames.isEmpty()) modeNames.add ("Default");
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::mode, 1 }, "Mode", modeNames, 0));

    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id::pitchBendRange, 1 }, "Pitch Bend Range", 1, 12, 2));

    // One float param per engine-driving control, keyed by label (deduped across
    // modes). Stored normalised 0..1; each mode maps it through its own control
    // range + bindings. The FIRST control seen for a given key sets the default.
    std::set<juce::String> seen;
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
            for (const auto& c : m.ui.tabs.getReference (0).controls)
                if (controlDrivesEngine (c))
                {
                    const auto cid = controlKey (c.label);
                    if (seen.insert (cid).second)
                        p.push_back (std::make_unique<AudioParameterFloat> (
                            ParameterID { cid, 1 }, c.label,
                            NormalisableRange<float> (0.0f, 1.0f), (float) normOf (c)));
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
        if (! controlDrivesEngine (c))
            continue;

        // One param per control (keyed by label); every binding maps that same
        // knob position through its own translation to its engine target.
        const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);
        const float  norm = raw (controlKey (c.label));

        for (const auto& b : c.bindings)
        {
            if (floatSpecFor (b.parameter) != nullptr)
            {
                applyBinding (engine, b, outputValue (b, norm, mn, mx));
            }
            else if (b.parameter == "TAG_VOLUME")
            {
                const float v = (float) outputValue (b, norm, mn, mx);
                for (int gi = 0; gi < mode.groups.size(); ++gi)
                    if (mode.groups.getReference (gi).tags.contains (b.identifier))
                        engine.setGroupTagVolume (gi, v);
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
            const auto& opt = menu.options.getReference (sel);
            engine.setSequencerIndexOffset (opt.seqIndex);   // chord-order menu (Omni)

            // Amp/cabinet-style menu: apply the option's cheap effect bindings here.
            // FX_IR_FILE (heavy IR reload) is applied on the message thread instead.
            for (const auto& b : opt.bindings)
            {
                if (! b.effectIndex)
                    continue;
                const int ei = *b.effectIndex;
                if (b.parameter == "ENABLED")
                    engine.setEffectEnabled (ei, bindingIsTrue (b));
                else if (b.parameter == "FX_MIX" || b.parameter == "FX_DRIVE" || b.parameter == "FX_OUTPUT_LEVEL")
                    engine.setEffectParam (ei, b.parameter, b.translationValue.toString().getFloatValue());
            }
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
            // Find the control this CC targets (by its engine parameter + group) and
            // drive that control's param. The converter precomputed normMin/normMax.
            if (mode.ui.tabs.isEmpty())
                continue;
            juce::String pid;
            for (const auto& c : mode.ui.tabs.getReference (0).controls)
            {
                if (! controlDrivesEngine (c)) continue;
                for (const auto& b : c.bindings)
                    if (b.parameter == cb.parameter
                        && (! cb.groupIndex || b.groupIndex.value_or (0) == *cb.groupIndex))
                    { pid = controlKey (c.label); break; }
                if (pid.isNotEmpty()) break;
            }
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
    if (controlDrivesEngine (c))
        ids.add (controlKey (c.label));   // one param per control, keyed by label
    return ids;
}

juce::String buttonParamId (const Button& b)
{
    const int k = buttonKindIndex (b);
    return k >= 0 ? juce::String (kBoolSpecs[k].id) : juce::String();
}

} // namespace dm::params
