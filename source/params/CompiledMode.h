#pragma once

// Compiled per-mode parameter plan.
//
// applyToEngine used to re-do, EVERY audio block: param-id string builds + APVTS map
// lookups per control/button, a ~21-branch string-compare ladder per binding, re-parsing
// of "in,out;…" translation tables, tag→group scans, and id→slot searches — thousands of
// string compares and hundreds of heap allocations per second on the audio thread.
//
// All of that is resolvable when the library loads (modes and the APVTS layout never
// change afterwards), so each mode is compiled ONCE, on the message thread, into a flat
// plan: raw std::atomic<float>* parameter pointers, pre-parsed translation tables, and
// pre-bound routing closures (plain int/enum captures). The per-block apply is then just
// atomic loads + arithmetic + engine setter calls. Build all modes up front and index by
// the active mode — no rebuild or retire machinery needed.

#include <juce_audio_processors/juce_audio_processors.h>
#include <model/Manifest.h>
#include <SamplerEngine.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace dm::params
{

class CompiledMode
{
public:
    /** Build the plan for one mode (message thread; APVTS layout must be final). */
    CompiledMode (const Mode& mode, juce::AudioProcessorValueTreeState& apvts);
    ~CompiledMode();

    /** Apply the current parameter values to the engine. AUDIO THREAD; allocation-free.
        Idempotent — call every block so a mode swap (which resets engine overrides) is
        re-honoured. `buttonClickSeq` orders button application by click recency (radio
        groups: last-clicked wins); see ManifestParameters.h for the full story. */
    void apply (SamplerEngine& engine, const std::atomic<std::uint32_t>* buttonClickSeq) const;

    /** Map incoming MIDI CC (mod wheel etc.) to their pre-resolved target params. */
    void applyCc (const juce::MidiBuffer& midi) const;

    /** Apply note key-switches (low keys selecting the chord-order menu). */
    void applyNoteSwitches (const juce::MidiBuffer& midi) const;

private:
    // A numeric value transform compiled from a binding's translation
    // (reversed / table / output range / factor over the control's min..max).
    struct Xform
    {
        enum class Kind { linear, outRange, tableLinear, tableNearest };
        Kind   kind = Kind::linear;
        bool   reversed = false;
        double mn = 0.0, mx = 1.0;         // control range
        double outMin = 0.0, outMax = 1.0; // outRange
        double factor = 1.0;               // linear
        std::vector<std::pair<double, double>> pts;   // pre-parsed table (document order)

        double eval (float norm) const noexcept;
    };

    using Route = std::function<void (SamplerEngine&, double value)>;

    struct Op { Xform xform; Route route; };                       // knob binding
    struct ControlItem { std::atomic<float>* param = nullptr; std::vector<Op> ops; };

    struct ButtonPlan
    {
        std::atomic<float>* param = nullptr;   // state index (null → state 0, single-state buttons)
        int numStates = 0;
        std::vector<std::vector<std::function<void (SamplerEngine&)>>> stateOps;   // values baked in
    };

    struct MenuPlan
    {
        std::atomic<float>* selParam = nullptr;   // chordOrder (null → option 0)
        struct Option { int seqIndex = 0; std::vector<std::function<void (SamplerEngine&)>> ops; };
        std::vector<Option> options;
    };

    struct CcPlan
    {
        int   cc = 1;
        float normMin = 0.0f, normMax = 1.0f;
        std::vector<juce::RangedAudioParameter*> targets;   // pre-matched controls
    };

    struct NoteSwitchPlan { int note = -1; int option = 0; };

    std::vector<ControlItem> controls;
    std::vector<ButtonPlan>  buttons;
    std::optional<MenuPlan>  menu;                 // first dropdown (current engine semantics)
    std::vector<CcPlan>      ccs;
    std::vector<NoteSwitchPlan> noteSwitches;
    juce::RangedAudioParameter* chordParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompiledMode)
};

} // namespace dm::params
