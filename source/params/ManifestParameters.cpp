#include "ManifestParameters.h"
#include "CompiledMode.h"
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
    { "ENV_ATTACK_CURVE",    "attackCurve",  "Attack Curve",  false },   // -100 log … +100 exp
    { "ENV_DECAY_CURVE",     "decayCurve",   "Decay Curve",   false },
    { "ENV_RELEASE_CURVE",   "releaseCurve", "Release Curve", false },
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

// Buttons get one param each — keyed by the button's manifest id when present
// (converter emits btn_<i>, so ids are unchanged for generated manifests; hand-
// authored ids survive reordering), else by tab position. An Int 0..numStates-1 so
// multi-state selectors (e.g. a 3-way stop selector) work, not just on/off.
static juce::String buttonId (int index) { return "btn_" + juce::String (index); }

// Param ids live in the APVTS ValueTree (XML attribute names) — sanitise a manifest
// id to the safe [A-Za-z0-9_] set. btn_<i>-style ids pass through unchanged.
static juce::String sanitizedParamId (const juce::String& id)
{
    auto s = id;
    for (int i = 0; i < s.length(); ++i)
    {
        const auto ch = s[i];
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                     || (ch >= '0' && ch <= '9') || ch == '_';
        if (! ok)
            s = s.replaceCharacter (ch, '_');
    }
    return s;
}

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

// "in,out;in,out;..." translation tables — parsed ONCE at compile time (CompiledMode)
// into point lists, in document order, then evaluated numerically per block.
static std::vector<std::pair<double, double>> parseTable (const juce::String& table)
{
    std::vector<std::pair<double, double>> pts;
    int start = 0;
    while (start < table.length())
    {
        int semi = table.indexOfChar (start, ';');
        if (semi < 0) semi = table.length();
        const int comma = table.indexOfChar (start, ',');
        if (comma > start && comma < semi)
            pts.emplace_back (table.substring (start, comma).getDoubleValue(),
                              table.substring (comma + 1, semi).getDoubleValue());
        start = semi + 1;
    }
    return pts;
}

// Piecewise-linear interpolation between the points (DecentSampler curve tables,
// e.g. a Saturation knob's FX_DRIVE). Clamps to the first/last output outside the
// table's input range.
static double evalPtsLinear (const std::vector<std::pair<double, double>>& pts, double x)
{
    double prevIn = 0.0, prevOut = 0.0; bool havePrev = false;
    for (const auto& [in, out] : pts)
    {
        if (x <= in)
        {
            if (! havePrev || ! (in > prevIn)) return out;   // before first point / vertical step
            const double t = (x - prevIn) / (in - prevIn);
            return prevOut + t * (out - prevOut);
        }
        prevIn = in; prevOut = out; havePrev = true;
    }
    return havePrev ? prevOut : 0.0;   // past the last point → last output
}

// Nearest-input lookup (DecentSampler step/radio tables for TAG_ENABLED selectors).
static double evalPtsNearest (const std::vector<std::pair<double, double>>& pts, double x)
{
    double bestOut = 0.0, bestDist = 1.0e18;
    for (const auto& [in, out] : pts)
    {
        const double d = in > x ? in - x : x - in;
        if (d < bestDist) { bestDist = d; bestOut = out; }
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

    {
        StringArray polyOpts;
        for (int i = 0; i < kNumPolyphonyChoices; ++i)
            polyOpts.add (String (kPolyphonyChoices[i]) + " voices");
        p.push_back (std::make_unique<AudioParameterChoice> (
            ParameterID { id::maxPolyphony, 1 }, "Max Polyphony", polyOpts, kNumPolyphonyChoices - 1));
    }

    // Sequencer tempo sync (settings menu): free-running by default; synced = 16th
    // steps at the host tempo (DAW builds; default follows the DAW) or a manual BPM.
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::seqTempoSync, 1 }, "Sequencer Tempo Sync", false));
    p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::seqSyncDaw, 1 }, "Sequencer Sync To DAW", true));
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::seqBpm, 1 }, "Sequencer BPM",
        juce::NormalisableRange<float> (30.0f, 300.0f, 0.1f), 120.0f));
    {
        StringArray nvOpts;
        for (int i = 0; i < kNumNoteValues; ++i)
            nvOpts.add (kNoteValueLabels[i]);
        p.push_back (std::make_unique<AudioParameterChoice> (
            ParameterID { id::seqNoteValue, 1 }, "Sequencer Note Value", nvOpts, kDefaultNoteValue));
    }

    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::masterTune, 1 }, "Master Tuning",
        juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f));
    p.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { id::velocityCurve, 1 }, "Velocity Curve",
        StringArray { "Soft", "Linear", "Hard" }, 1));

    // Shared-air simulation toggle — only for libraries that declare it (Elektrisk).
    if (lib.airSupply)
        p.push_back (std::make_unique<AudioParameterBool> (ParameterID { id::airSupply, 1 }, "Air Supply", true));

    // Separate up/down wheel ranges (settings menu). 0 disables that direction.
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id::pitchBendUp,   1 }, "Pitch Bend Up",   0, 24, 2));
    p.push_back (std::make_unique<AudioParameterInt> (ParameterID { id::pitchBendDown, 1 }, "Pitch Bend Down", 0, 24, 2));

    // User master output fader (dB, post-everything). Independent of the preset's
    // AMP_VOLUME master. -60 dB = silence, 0 dB = unity, +6 dB = boost.
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::masterOutput, 1 }, "Master Output",
        juce::NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f));

    // Global drift wheels (0..1, off by default), right of the keyboard. Independent
    // per-voice pitch + volume wander in every plugin.
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::pitchDrift, 1 }, "Pitch Drift",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { id::volumeDrift, 1 }, "Volume Drift",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    p.push_back (std::make_unique<AudioParameterBool> (
        ParameterID { id::skipMuted, 1 }, "Skip Silent Groups", lib.polySaveDefault));

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

    // Button params: one Int per distinct button param id (0..numStates-1), deduped
    // across modes (max state count + the authored default of the first seen). Lane
    // order = first-seen order (= button order for generated btn_<i> ids).
    StringArray btnOrder;
    std::map<juce::String, int> btnStates, btnDefault, btnNumber;
    for (const auto& m : lib.modes)
        if (! m.ui.tabs.isEmpty())
        {
            const auto& btns = m.ui.tabs.getReference (0).buttons;
            for (int i = 0; i < btns.size(); ++i)
            {
                const auto& btn = btns.getReference (i);
                const auto pid  = buttonParamId (btn, i);
                if (! btnStates.count (pid))
                {
                    btnOrder.add (pid);
                    btnNumber[pid]  = i + 1;   // DAW display name stays "Button <n>"
                    btnDefault[pid] = btn.value.value_or (0);
                    btnStates[pid]  = 0;
                }
                btnStates[pid] = juce::jmax (btnStates[pid], btn.states.size());
            }
        }
    for (const auto& pid : btnOrder)
        if (const int n = btnStates[pid]; n >= 2)
            p.push_back (std::make_unique<AudioParameterInt> (
                ParameterID { pid, 1 }, "Button " + String (btnNumber[pid]),
                0, n - 1, juce::jlimit (0, n - 1, btnDefault[pid])));

    // Chord-order menu (first dropdown found).
    auto chordOpts = firstMenuOptions (lib);
    if (! chordOpts.isEmpty())
        p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { id::chordOrder, 1 }, "Chord Order", chordOpts, 0));

    return { p.begin(), p.end() };
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CompiledMode — see CompiledMode.h. Built once per mode on the message thread;
// apply()/applyCc()/applyNoteSwitches() are the audio-thread replacements for the
// old per-block applyToEngine/applyCcToParams/applyNoteSwitches (whose string
// ladders + table re-parsing this compiles away).
// ---------------------------------------------------------------------------

double CompiledMode::Xform::eval (float norm) const noexcept
{
    const double n = reversed ? 1.0 - (double) norm : (double) norm;
    switch (kind)
    {
        case Kind::tableLinear:  return evalPtsLinear  (pts, mn + n * (mx - mn));
        case Kind::tableNearest: return evalPtsNearest (pts, mn + n * (mx - mn));
        case Kind::outRange:     return outMin + n * (outMax - outMin);
        case Kind::linear:       break;
    }
    return (mn + n * (mx - mn)) * factor;
}

CompiledMode::CompiledMode (const Mode& mode, juce::AudioProcessorValueTreeState& apvts)
{
    // Note key-switches are tab-independent.
    chordParam = apvts.getParameter (id::chordOrder);
    for (const auto& ks : mode.menuKeySwitches)
        noteSwitches.push_back ({ ks.note, ks.option });

    if (mode.ui.tabs.isEmpty())
        return;
    const auto& tab = mode.ui.tabs.getReference (0);

    // The value transform, mirroring the old outputValue() exactly. TAG_ENABLED table
    // selectors use NEAREST lookup on the raw selector value (no reversal) — also as before.
    auto makeXform = [] (const Binding& b, double mn, double mx, bool nearestTable)
    {
        Xform x;
        x.mn = mn; x.mx = mx;
        if (nearestTable)
        {
            x.kind = Xform::Kind::tableNearest;
            x.pts  = parseTable (b.translationTable);
            return x;
        }
        x.reversed = b.translationReversed;
        if (b.translationTable.isNotEmpty())
        {
            x.kind = Xform::Kind::tableLinear;
            x.pts  = parseTable (b.translationTable);
        }
        else if (b.translationOutputMin && b.translationOutputMax)
        {
            x.kind   = Xform::Kind::outRange;
            x.outMin = *b.translationOutputMin;
            x.outMax = *b.translationOutputMax;
        }
        else
        {
            x.kind   = Xform::Kind::linear;
            x.factor = b.factor.value_or (1.0);
        }
        return x;
    };

    auto tagGroups = [&mode] (const juce::String& tag)
    {
        std::vector<int> g;
        for (int gi = 0; gi < mode.groups.size(); ++gi)
            if (mode.groups.getReference (gi).tags.contains (tag))
                g.push_back (gi);
        return g;
    };

    // The routing half of the old applyBinding ladder, resolved NOW: parameter name →
    // engine setter, id/index → slot, level → group vs instrument. Returns nullptr for
    // bindings the engine doesn't act on (same as applyBinding's silent fall-through).
    auto compileRoute = [&mode] (const Binding& b) -> Route
    {
        using E = SamplerEngine;
        using P = FxChain::FxParam;
        const auto& p  = b.parameter;
        const int  fx  = effectSlotFor (mode, b);   // instrument effect slot (id-resolved)
        const int  grp = groupSlotFor (mode, b);    // group index (id-resolved)
        const bool groupEffect = b.level == "group" && grp >= 0 && b.effectIndex.has_value();
        const bool groupLevel  = b.level == "group" && grp >= 0;
        const int  gei = b.effectIndex.value_or (0);

        // FX param routed group-chain → addressed instrument effect → semantic fallback.
        auto fxRoute = [&] (P fp, Route fallback, bool allowGroup = true) -> Route
        {
            if (allowGroup && groupEffect)
                return [grp, gei, fp] (E& e, double v) { e.setGroupEffectParam (grp, gei, fp, (float) v); };
            if (fx >= 0)
                return [fx, fp] (E& e, double v) { e.setEffectParam (fx, fp, (float) v); };
            return fallback;
        };

        if (p == "FX_FILTER_FREQUENCY") return fxRoute (P::filterFrequency, [] (E& e, double v) { e.setLowpassFrequency ((float) v); });
        if (p == "FX_FILTER_RESONANCE") return fxRoute (P::filterResonance, nullptr);
        if (p == "FX_MOD_RATE")         return fxRoute (P::modRate,  nullptr, false);
        if (p == "FX_MOD_DEPTH")        return fxRoute (P::modDepth, nullptr, false);
        if (p == "FX_FEEDBACK")         return fxRoute (P::feedback, nullptr, false);
        if (p == "FX_MIX")              return fxRoute (P::mix,   [] (E& e, double v) { e.setReverbMix ((float) v); });
        if (p == "FX_DRIVE")            return fxRoute (P::drive, [] (E& e, double v) { e.setWaveShaperDrive ((float) v); });
        if (p == "FX_OUTPUT_LEVEL")     return fxRoute (P::outputLevel, [] (E& e, double v) { e.setReverbWetGainDb ((float) v); }, false);
        if (p == "LEVEL")
        {
            // group-level gain: per-group chain if it has one (else falls back to a
            // per-group output gain inside the engine); instrument gain → by id.
            if (groupLevel) return [grp, gei] (E& e, double v) { e.setGroupEffectParam (grp, gei, P::level, (float) v); };
            if (fx >= 0)    return [fx]       (E& e, double v) { e.setEffectParam (fx, P::level, (float) v); };
            return [] (E& e, double v) { e.setGain ((float) v); };
        }
        if (p == "SEQ_PLAYBACK_RATE")   return [] (E& e, double v) { e.setSequencerRate (v); };
        // ENV_* with a group target → that group only; without → the global amp envelope.
        if (p == "ENV_ATTACK")  { if (groupLevel) return [grp] (E& e, double v) { e.setGroupAmpAttack  (grp, (float) v); }; return [] (E& e, double v) { e.setAmpAttack  ((float) v); }; }
        if (p == "ENV_DECAY")   { if (groupLevel) return [grp] (E& e, double v) { e.setGroupAmpDecay   (grp, (float) v); }; return [] (E& e, double v) { e.setAmpDecay   ((float) v); }; }
        if (p == "ENV_SUSTAIN") { if (groupLevel) return [grp] (E& e, double v) { e.setGroupAmpSustain (grp, (float) v); }; return [] (E& e, double v) { e.setAmpSustain ((float) v); }; }
        if (p == "ENV_RELEASE") { if (groupLevel) return [grp] (E& e, double v) { e.setGroupAmpRelease (grp, (float) v); }; return [] (E& e, double v) { e.setAmpRelease ((float) v); }; }
        if (p == "ENV_ATTACK_CURVE")  return [] (E& e, double v) { e.setAmpAttackCurve  ((float) v); };
        if (p == "ENV_DECAY_CURVE")   return [] (E& e, double v) { e.setAmpDecayCurve   ((float) v); };
        if (p == "ENV_RELEASE_CURVE") return [] (E& e, double v) { e.setAmpReleaseCurve ((float) v); };
        // DecentSampler pan is -100..+100 → engine -1..+1 (double-track "Stereo" button).
        if (p == "PAN")               { if (grp >= 0) return [grp] (E& e, double v) { e.setGroupPan (grp, (float) v / 100.0f); }; return nullptr; }
        if (p == "AMP_VOLUME")
        {
            if (grp >= 0) return [grp] (E& e, double v) { e.setGroupVolume (grp, (float) v); };
            return [] (E& e, double v) { e.setMasterVolume ((float) v); };
        }
        if (p == "GROUP_TUNING")      { const int g = grp >= 0 ? grp : 0; return [g] (E& e, double v) { e.setGroupTuning (g, (float) v); }; }
        if (p == "AMP_VEL_TRACK")
        {
            // Group-level binding drives only that group's velocity response (e.g.
            // SubC's Saturation fading the clean-dyn group's tracking); instrument
            // level keeps the global override (the 4-track velocity switches).
            if (grp >= 0) return [grp] (E& e, double v) { e.setGroupAmpVelTrack (grp, (float) v); };
            return [] (E& e, double v) { e.setAmpVelTrack ((float) v); };
        }
        if (p == "MOD_AMOUNT")        { const int m = modSlotFor (mode, b); return [m] (E& e, double v) { e.setLfoDepth (m, (float) v); }; }
        if (p == "FREQUENCY")         { const int m = modSlotFor (mode, b); return [m] (E& e, double v) { e.setLfoRate  (m, (float) v); }; }
        if (p == "ENABLED")
        {
            if (b.level == "group" && grp >= 0) return [grp] (E& e, double v) { e.setGroupEnabled (grp, v > 0.5); };
            if (fx >= 0)                        return [fx]  (E& e, double v) { e.setEffectEnabled (fx, v > 0.5); };
            return nullptr;
        }
        return nullptr;   // not an engine parameter
    };

    // ---- knobs/faders ------------------------------------------------------
    for (const auto& c : tab.controls)
    {
        if (! controlDrivesEngine (c))
            continue;

        ControlItem item;
        item.param = apvts.getRawParameterValue (controlKey (c.label));
        if (item.param == nullptr)
            continue;
        const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);

        for (const auto& b : c.bindings)
        {
            // Tag-addressed volume: TAG_VOLUME, or AMP_VOLUME at level="tag" — resolve
            // the tag to its groups NOW (was a per-block scan).
            const bool tagVolume = b.parameter == "TAG_VOLUME"
                                || (b.parameter == "AMP_VOLUME" && b.level == "tag" && b.identifier.isNotEmpty());
            if (tagVolume)
            {
                auto groups = tagGroups (b.identifier);
                if (groups.empty()) continue;
                Op op;
                op.xform = makeXform (b, mn, mx, false);
                op.route = [gs = std::move (groups)] (SamplerEngine& e, double v)
                {
                    for (int g : gs) e.setGroupTagVolume (g, (float) v);
                };
                item.ops.push_back (std::move (op));
            }
            else if (b.parameter == "TAG_ENABLED")
            {
                // Table selector → nearest lookup, on at > 0.5. Linear/no-table (e.g.
                // StyloPoly's noise-on knob) → mapped value, on as it leaves 0.
                auto groups = tagGroups (b.identifier);
                if (groups.empty()) continue;
                const bool useTable = b.translationTable.isNotEmpty();
                Op op;
                op.xform = makeXform (b, mn, mx, useTable);
                const double thresh = useTable ? 0.5 : 1.0e-6;
                op.route = [gs = std::move (groups), thresh] (SamplerEngine& e, double v)
                {
                    const bool on = v > thresh;
                    for (int g : gs) e.setGroupEnabled (g, on);
                };
                item.ops.push_back (std::move (op));
            }
            else if (floatSpecFor (b.parameter) != nullptr)
            {
                if (auto route = compileRoute (b))
                    item.ops.push_back ({ makeXform (b, mn, mx, false), std::move (route) });
                // A StrumSpeed knob also reports its NORMALISED position: tempo-synced
                // mode sweeps the note-value table with it (the raw steps/s value
                // would make the note-value mapping depend on the current BPM).
                if (b.parameter == "SEQ_PLAYBACK_RATE")
                {
                    Op op;
                    op.xform.kind   = Xform::Kind::outRange;
                    op.xform.outMin = 0.0;
                    op.xform.outMax = 1.0;
                    op.route = [] (SamplerEngine& e, double v) { e.setSequencerRateNorm (v); };
                    item.ops.push_back (std::move (op));
                }
            }
        }
        if (! item.ops.empty())
            controls.push_back (std::move (item));
    }

    // ---- buttons (fixed values baked in) ------------------------------------
    for (int i = 0; i < tab.buttons.size(); ++i)
    {
        const auto& btn = tab.buttons.getReference (i);
        ButtonPlan bp;
        bp.numStates = btn.states.size();
        bp.param = apvts.getRawParameterValue (buttonParamId (btn, i));   // null for single-state buttons → state 0

        for (const auto& st : btn.states)
        {
            std::vector<std::function<void (SamplerEngine&)>> ops;
            for (const auto& b : st.bindings)
            {
                // PATH (sample swap) and FX_IR_FILE (convolution IR) are applied on the
                // message thread by the processor (async IR reload), not from here.
                if (b.parameter == "PATH" || b.parameter == "FX_IR_FILE")
                    continue;

                if (b.parameter == "TAG_ENABLED")
                {
                    // Enable/disable every group carrying the named tag (e.g. a damping
                    // switch swapping "damped"/"sustained" groups).
                    auto groups = tagGroups (b.identifier);
                    if (groups.empty()) continue;
                    const bool on = bindingIsTrue (b);
                    ops.push_back ([gs = std::move (groups), on] (SamplerEngine& e)
                    {
                        for (int g : gs) e.setGroupEnabled (g, on);
                    });
                    continue;
                }

                // Everything else (ENABLED, FX_DRIVE, MOD_AMOUNT, …) carries a fixed value.
                if (auto route = compileRoute (b))
                {
                    const double v = buttonBindingValue (b);
                    ops.push_back ([r = std::move (route), v] (SamplerEngine& e) { r (e, v); });
                }
            }
            bp.stateOps.push_back (std::move (ops));
        }
        buttons.push_back (std::move (bp));
    }

    // ---- first dropdown menu (chord order / amp-cabinet selector) -----------
    for (const auto& m : tab.menus)
        if (! m.options.isEmpty())
        {
            MenuPlan mp;
            mp.selParam = apvts.getRawParameterValue (id::chordOrder);
            for (const auto& opt : m.options)
            {
                MenuPlan::Option o;
                o.seqIndex = opt.seqIndex;
                for (const auto& b : opt.bindings)
                {
                    // Cheap FX bindings only; FX_IR_FILE (heavy IR reload) stays on the
                    // message thread. Effect resolved by id with effectIndex fallback.
                    const int ei = effectSlotFor (mode, b);
                    if (ei < 0)
                        continue;
                    using P = FxChain::FxParam;
                    if (b.parameter == "ENABLED")
                    {
                        const bool on = bindingIsTrue (b);
                        o.ops.push_back ([ei, on] (SamplerEngine& e) { e.setEffectEnabled (ei, on); });
                    }
                    else if (b.parameter == "FX_MIX" || b.parameter == "FX_DRIVE" || b.parameter == "FX_OUTPUT_LEVEL")
                    {
                        const float v = b.translationValue.toString().getFloatValue();
                        const P fp = b.parameter == "FX_MIX" ? P::mix
                                   : b.parameter == "FX_DRIVE" ? P::drive : P::outputLevel;
                        o.ops.push_back ([ei, fp, v] (SamplerEngine& e) { e.setEffectParam (ei, fp, v); });
                    }
                }
                mp.options.push_back (std::move (o));
            }
            menu = std::move (mp);
            break;
        }

    // ---- CC bindings → pre-matched target params -----------------------------
    for (const auto& cb : mode.ccBindings)
    {
        CcPlan plan;
        plan.cc      = cb.cc;
        plan.normMin = (float) cb.normMin;
        plan.normMax = (float) cb.normMax;

        // When the CC binding names its target (id, or controlIndex in regenerated
        // manifests) drive ONLY that control; legacy fallback drives every control
        // sharing the parameter+group. Same matching as before, resolved once.
        for (const auto& c : tab.controls)
        {
            if (! controlDrivesEngine (c)) continue;

            bool matches = false;
            if (cb.targetId.isNotEmpty())
            {
                matches = (c.id == cb.targetId);
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
                    plan.targets.push_back (prm);
        }
        if (! plan.targets.empty())
            ccs.push_back (std::move (plan));
    }
}

CompiledMode::~CompiledMode() = default;

void CompiledMode::apply (SamplerEngine& engine, const std::atomic<std::uint32_t>* buttonClickSeq) const
{
    for (const auto& item : controls)
    {
        const float norm = item.param->load();
        for (const auto& op : item.ops)
            op.route (engine, op.xform.eval (norm));
    }

    // Buttons oldest-click-first (never-clicked keep index order): among buttons
    // targeting the SAME effect (a radio group) the most-recently-clicked wins.
    constexpr int kMaxBtn = kMaxUiButtons;   // shared cap (params::kMaxUiButtons)
    const int nB = juce::jmin ((int) buttons.size(), kMaxBtn);
    int order[kMaxBtn];
    std::uint32_t seq[kMaxBtn];
    for (int i = 0; i < nB; ++i) { order[i] = i; seq[i] = buttonClickSeq ? buttonClickSeq[i].load() : 0u; }
    std::stable_sort (order, order + nB, [&seq] (int a, int b) { return seq[a] < seq[b]; });

    for (int k = 0; k < nB; ++k)
    {
        const auto& bp = buttons[(size_t) order[k]];
        if (bp.stateOps.empty()) continue;
        const int st = juce::jlimit (0, (int) bp.stateOps.size() - 1,
                                     bp.param != nullptr ? (int) bp.param->load() : 0);
        for (const auto& op : bp.stateOps[(size_t) st])
            op (engine);
    }

    if (menu.has_value() && ! menu->options.empty())
    {
        const int sel = juce::jlimit (0, (int) menu->options.size() - 1,
                                      menu->selParam != nullptr ? (int) menu->selParam->load() : 0);
        const auto& opt = menu->options[(size_t) sel];
        engine.setSequencerIndexOffset (opt.seqIndex);   // chord-order menu (Omni)
        for (const auto& op : opt.ops)
            op (engine);
    }
}

void CompiledMode::applyCc (const juce::MidiBuffer& midi) const
{
    if (ccs.empty())
        return;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isController())
            continue;

        const int   cc = msg.getControllerNumber();
        const float v  = (float) msg.getControllerValue() / 127.0f;

        for (const auto& plan : ccs)
        {
            if (plan.cc != cc)
                continue;
            const float norm = juce::jlimit (0.0f, 1.0f, plan.normMin + v * (plan.normMax - plan.normMin));
            for (auto* prm : plan.targets)
                prm->setValueNotifyingHost (norm);
        }
    }
}

void CompiledMode::applyNoteSwitches (const juce::MidiBuffer& midi) const
{
    if (noteSwitches.empty() || chordParam == nullptr)
        return;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isNoteOn())
            continue;
        const int note = msg.getNoteNumber();
        for (const auto& ks : noteSwitches)
            if (ks.note == note)
                chordParam->setValueNotifyingHost (chordParam->convertTo0to1 ((float) ks.option));
    }
}

juce::StringArray controlParamIds (const Control& c)
{
    juce::StringArray ids;
    if (controlDrivesEngine (c))
        ids.add (controlKey (c.label));   // one param per control, keyed by label
    return ids;
}

juce::String buttonParamId (const Button& button, int fallbackIndex)
{
    if (button.id.isNotEmpty())
        return sanitizedParamId (button.id);
    return buttonId (fallbackIndex);
}

} // namespace dm::params
