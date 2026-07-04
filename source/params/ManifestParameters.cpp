#include "ManifestParameters.h"
#include <set>
#include <map>
#include <algorithm>
#include <cstdint>

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
    { "FX_FILTER_RESONANCE", "resonance",  "Resonance",   false },
    { "FX_MOD_RATE",         "fxModRate",  "Mod Rate",    false },   // chorus/phaser rate
    { "FX_MOD_DEPTH",        "fxModDepth", "Mod Depth",   false },   // chorus/phaser depth
    { "FX_FEEDBACK",         "fxFeedback", "Feedback",    false },   // phaser feedback / chorus
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
    { "MOD_AMOUNT",          "modAmount",  "Mod Amount",  false },   // LFO depth
    { "FREQUENCY",           "lfoRate",    "LFO Rate",    false },   // LFO rate (Hz)
};

// With per-control keying the baseId/perGroup/baseName fields are vestigial — each
// control now owns one param keyed by its label (see controlKey). The registry is
// kept only as the set of float engine parameters applyBinding knows how to route.

// Buttons get one param each, keyed by their index in the tab (btn_<i>), an Int
// 0..numStates-1 so multi-state selectors (e.g. a 3-way stop selector) work, not
// just on/off. Helpers below.
static juce::String buttonId (int index) { return "btn_" + juce::String (index); }

static const FloatSpec* floatSpecFor (const juce::String& parameter)
{
    for (const auto& s : kFloatSpecs)
        if (parameter == s.engineParam)
            return &s;
    return nullptr;
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
    // translationReversed flips the knob's sense — apply it once here so it works
    // for ALL binding kinds (table, explicit output range, and plain linear).
    if (b.translationReversed) norm = 1.0f - norm;

    if (b.translationTable.isNotEmpty())
        return evalTableLinear (b.translationTable, mn + (double) norm * (mx - mn));
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
// Resolve a binding's instrument-effect target to a slot index: prefer the id
// (targetId), falling back to the legacy effectIndex during the id migration.
static int effectSlotFor (const Mode& mode, const Binding& b)
{
    if (b.targetId.isNotEmpty())
        for (int i = 0; i < mode.effects.size(); ++i)
            if (mode.effects.getReference (i).id == b.targetId)
                return i;
    return b.effectIndex.value_or (-1);
}

// Resolve a binding's group target to a group index: prefer the id (targetId → group
// uid), falling back to the legacy groupIndex during the id migration.
static int groupSlotFor (const Mode& mode, const Binding& b)
{
    if (b.targetId.isNotEmpty())
        for (int i = 0; i < mode.groups.size(); ++i)
            if (mode.groups.getReference (i).uid == b.targetId)
                return i;
    return b.groupIndex.value_or (-1);
}

// Resolve a MOD_AMOUNT/FREQUENCY binding's modulator target to an index: prefer the id
// (targetId → modulator id), falling back to the legacy position (= modulatorIndex).
static int modSlotFor (const Mode& mode, const Binding& b)
{
    if (b.targetId.isNotEmpty())
        for (int i = 0; i < mode.modulators.size(); ++i)
            if (mode.modulators.getReference (i).id == b.targetId)
                return i;
    return b.position.value_or (0);
}

static void applyBinding (SamplerEngine& eng, const Mode& mode, const Binding& b, double value)
{
    const auto& p = b.parameter;
    const int  fx  = effectSlotFor (mode, b);   // instrument effect slot (id-resolved)
    const int  grp = groupSlotFor (mode, b);    // group index (id-resolved)
    // Group-level effect (level="group" + effectIndex): a per-group insert chain
    // (organ swell filter, loudness gain, …) — route to that group's chain by (group, slot).
    const bool groupEffect = b.level == "group" && grp >= 0 && b.effectIndex;
    const bool groupLevel  = b.level == "group" && grp >= 0;

    if (p == "FX_FILTER_FREQUENCY")
    {
        // Address the specific effect when its id resolves — a mode can have several
        // filters (e.g. a lowpass AND a highpass); routing all of them to "the lowpass"
        // would let one clobber the other. Fall back to the first lowpass only when no
        // target is given.
        if      (groupEffect)   eng.setGroupEffectParam (grp, *b.effectIndex, p, (float) value);
        else if (fx >= 0)       eng.setEffectParam (fx, p, (float) value);
        else                    eng.setLowpassFrequency ((float) value);
    }
    // VCF resonance + chorus/phaser modulation params: route to the addressed effect.
    else if (p == "FX_FILTER_RESONANCE") { if (groupEffect) eng.setGroupEffectParam (grp, *b.effectIndex, p, (float) value); else if (fx >= 0) eng.setEffectParam (fx, p, (float) value); }
    else if (p == "FX_MOD_RATE" || p == "FX_MOD_DEPTH" || p == "FX_FEEDBACK") { if (fx >= 0) eng.setEffectParam (fx, p, (float) value); }
    // Effect params: address a specific instrument effect by id when known;
    // FxChain routes FX_OUTPUT_LEVEL by the slot's kind (wave_shaper vs convolution).
    else if (p == "FX_MIX")              { if (groupEffect) eng.setGroupEffectParam (grp, *b.effectIndex, p, (float) value); else if (fx >= 0) eng.setEffectParam (fx, p, (float) value); else eng.setReverbMix ((float) value); }
    else if (p == "FX_DRIVE")            { if (groupEffect) eng.setGroupEffectParam (grp, *b.effectIndex, p, (float) value); else if (fx >= 0) eng.setEffectParam (fx, p, (float) value); else eng.setWaveShaperDrive ((float) value); }
    else if (p == "FX_OUTPUT_LEVEL")     { if (fx >= 0) eng.setEffectParam (fx, p, (float) value); else eng.setReverbWetGainDb ((float) value); }
    else if (p == "LEVEL")
    {
        // group-level gain: per-group chain if it has one (else falls back to a
        // per-group output gain inside the engine); instrument gain → by id.
        if      (groupLevel)    eng.setGroupEffectParam (grp, b.effectIndex.value_or (0), p, (float) value);
        else if (fx >= 0)       eng.setEffectParam (fx, p, (float) value);
        else                    eng.setGain ((float) value);
    }
    else if (p == "SEQ_PLAYBACK_RATE")   eng.setSequencerRate (value);
    // ENV_* with a group target → that group only (e.g. organ "loudness" lengthening
    // just the reed groups' attack); without → the global amp envelope.
    else if (p == "ENV_ATTACK")          { if (groupLevel) eng.setGroupAmp (grp, p, (float) value); else eng.setAmpAttack ((float) value); }
    else if (p == "ENV_DECAY")           { if (groupLevel) eng.setGroupAmp (grp, p, (float) value); else eng.setAmpDecay ((float) value); }
    else if (p == "ENV_SUSTAIN")         { if (groupLevel) eng.setGroupAmp (grp, p, (float) value); else eng.setAmpSustain ((float) value); }
    else if (p == "ENV_RELEASE")         { if (groupLevel) eng.setGroupAmp (grp, p, (float) value); else eng.setAmpRelease ((float) value); }
    else if (p == "AMP_VOLUME")          { if (grp >= 0) eng.setGroupVolume (grp, (float) value); else eng.setMasterVolume ((float) value); }
    else if (p == "GROUP_TUNING")        eng.setGroupTuning (grp >= 0 ? grp : 0, (float) value);
    else if (p == "AMP_VEL_TRACK")       eng.setAmpVelTrack ((float) value);
    else if (p == "MOD_AMOUNT")          eng.setLfoDepth (modSlotFor (mode, b), (float) value);
    else if (p == "FREQUENCY")           eng.setLfoRate  (modSlotFor (mode, b), (float) value);
    else if (p == "ENABLED")
    {
        const bool on = value > 0.5;
        if (b.level == "group" && grp >= 0) eng.setGroupEnabled (grp, on);
        else if (fx >= 0)                   eng.setEffectEnabled (fx, on);
    }
    // AMP_VEL_TRACK etc.: no engine runtime param yet.
}

static bool bindingIsTrue (const Binding& b)
{
    if (b.translationValue.isBool()) return (bool) b.translationValue;
    const auto s = b.translationValue.toString();
    if (s.equalsIgnoreCase ("true"))  return true;
    if (s.equalsIgnoreCase ("false")) return false;
    return s.getFloatValue() > 0.5f;   // numeric "1"/"0"
}

// A button-state binding carries a fixed value in translationValue → resolve to a number.
static double buttonBindingValue (const Binding& b)
{
    if (b.translationValue.isBool()) return (bool) b.translationValue ? 1.0 : 0.0;
    const auto s = b.translationValue.toString();
    if (s.equalsIgnoreCase ("true"))  return 1.0;
    if (s.equalsIgnoreCase ("false")) return 0.0;
    return s.getDoubleValue();   // "4.11", "0.2", "4277", "1"/"0", …
}

static void applyButtonState (SamplerEngine& eng, const Mode& mode, const ButtonState& st)
{
    for (const auto& b : st.bindings)
    {
        // PATH (sample swap) and FX_IR_FILE (convolution IR) are applied on the message
        // thread by the processor (async IR reload), not from here. Everything else —
        // including MOD_AMOUNT/FREQUENCY, which now carry the modulator index via
        // `position` (from DecentSampler's modulatorIndex) — routes through applyBinding,
        // so e.g. a hi-fi/lo-fi switch can zero the lo-fi modulators in hi-fi.
        if (b.parameter == "PATH" || b.parameter == "FX_IR_FILE") continue;

        if (b.parameter == "TAG_ENABLED")
        {
            // Enable/disable every group carrying the named tag (e.g. a damping switch
            // swapping between "damped" and "sustained" groups). Not handled by applyBinding.
            const bool on = bindingIsTrue (b);
            for (int gi = 0; gi < mode.groups.size(); ++gi)
                if (mode.groups.getReference (gi).tags.contains (b.identifier))
                    eng.setGroupEnabled (gi, on);
            continue;
        }

        // Everything else (ENABLED, FX_DRIVE, FX_OUTPUT_LEVEL, FX_FILTER_FREQUENCY,
        // AMP_VEL_TRACK, MOD_AMOUNT, …) carries a fixed value → route through the same
        // type-aware path the UI controls use, so a hi-fi/lo-fi switch actually applies
        // the wave_shaper's drive + output level (and any filter settings).
        applyBinding (eng, mode, b, buttonBindingValue (b));
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

    // User master output fader (dB, post-everything). Independent of the preset's
    // AMP_VOLUME master. -60 dB = silence, 0 dB = unity, +6 dB = boost.
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::masterOutput, 1 }, "Master Output",
        juce::NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f));

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

    // Button params: one Int per button index (0..numStates-1), deduped across
    // modes (max state count + the authored default). Lane order = button order.
    std::map<int, int> btnStates, btnDefault;
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
        {
            const auto& btns = m.ui.tabs.getReference (0).buttons;
            for (int i = 0; i < btns.size(); ++i)
            {
                const int n = btns.getReference (i).states.size();
                btnStates[i]  = juce::jmax (btnStates.count (i) ? btnStates[i] : 0, n);
                if (! btnDefault.count (i))
                    btnDefault[i] = btns.getReference (i).value.value_or (0);
            }
        }
    for (const auto& [idx, n] : btnStates)
        if (n >= 2)
            p.push_back (std::make_unique<AudioParameterInt> (
                ParameterID { buttonId (idx), 1 }, "Button " + String (idx + 1),
                0, n - 1, juce::jlimit (0, n - 1, btnDefault[idx])));

    // Chord-order menu (first dropdown found).
    auto chordOpts = firstMenuOptions (lib);
    if (! chordOpts.isEmpty())
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::chordOrder, 1 }, "Chord Order", chordOpts, 0));

    return { p.begin(), p.end() };
}

// ---------------------------------------------------------------------------

void applyToEngine (SamplerEngine& engine, const Mode& mode,
                    juce::AudioProcessorValueTreeState& apvts,
                    const std::atomic<std::uint32_t>* buttonClickSeq)
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
            // Tag-addressed volume: TAG_VOLUME, or AMP_VOLUME at level="tag" (e.g. the
            // Mechanical Noise knob targets the key-on/key-off tags). Resolve the tag
            // to its groups — NOT master (which is AMP_VOLUME with no group/tag).
            const bool tagVolume = b.parameter == "TAG_VOLUME"
                                || (b.parameter == "AMP_VOLUME" && b.level == "tag" && b.identifier.isNotEmpty());
            if (tagVolume)
            {
                const float v = (float) outputValue (b, norm, mn, mx);
                for (int gi = 0; gi < mode.groups.size(); ++gi)
                    if (mode.groups.getReference (gi).tags.contains (b.identifier))
                        engine.setGroupTagVolume (gi, v);
            }
            else if (floatSpecFor (b.parameter) != nullptr)
            {
                applyBinding (engine, mode, b, outputValue (b, norm, mn, mx));
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

    // Apply buttons oldest-click-first (never-clicked keep index order). Among any set
    // of buttons targeting the SAME effect (a radio group, e.g. the ensemble O/Acc/Solo/
    // Organ), the most-recently-clicked is applied LAST and wins — and clicking an
    // unrelated button doesn't change the ensemble's winner. Snapshot the seq first so
    // the sort sees a stable ordering.
    constexpr int kMaxBtn = 64;
    const int nB = juce::jmin (tab.buttons.size(), kMaxBtn);
    int order[kMaxBtn];
    std::uint32_t seq[kMaxBtn];
    for (int i = 0; i < nB; ++i) { order[i] = i; seq[i] = buttonClickSeq ? buttonClickSeq[i].load() : 0u; }
    std::stable_sort (order, order + nB, [&seq] (int a, int b) { return seq[a] < seq[b]; });

    for (int k = 0; k < nB; ++k)
    {
        const int i = order[k];
        const auto& btn = tab.buttons.getReference (i);
        if (btn.states.isEmpty()) continue;
        const int stateIndex = juce::jlimit (0, btn.states.size() - 1, (int) raw (buttonId (i)));
        applyButtonState (engine, mode, btn.states.getReference (stateIndex));
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
            if (mode.ui.tabs.isEmpty())
                continue;
            const float norm = juce::jlimit (0.0f, 1.0f,
                                             (float) (cb.normMin + v * (cb.normMax - cb.normMin)));
            // When the CC binding names its specific target control (controlIndex —
            // regenerated manifests), drive ONLY that control. Otherwise (legacy
            // manifests that lost the index) fall back to driving EVERY control sharing
            // the parameter+group — which is how a mod wheel→MOD_AMOUNT opened both of
            // the 4-track/Wurli tremolo-depth knobs. Precise targeting fixes libraries
            // like EDB-Orgel where FREQUENCY is shared by the vibrato, every tremolo LFO,
            // and a stray velocity control — the mod wheel must move only the vibrato.
            for (const auto& c : mode.ui.tabs.getReference (0).controls)
            {
                if (! controlDrivesEngine (c)) continue;

                bool matches = false;
                if (cb.targetId.isNotEmpty())
                {
                    matches = (c.id == cb.targetId);        // id-based target (preferred)
                }
                else if (cb.controlIndex)
                {
                    matches = c.controlIndex && *c.controlIndex == *cb.controlIndex;
                }
                else
                {
                    for (const auto& b : c.bindings)
                        if (b.parameter == cb.parameter
                            && (! cb.groupIndex || b.groupIndex.value_or (0) == *cb.groupIndex))
                        { matches = true; break; }
                }

                if (matches)
                    if (auto* prm = apvts.getParameter (controlKey (c.label)))
                        prm->setValueNotifyingHost (norm);
            }
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

juce::String buttonParamId (int buttonIndex)
{
    return buttonId (buttonIndex);
}

} // namespace dm::params
