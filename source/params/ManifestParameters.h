#pragma once

// Manifest-driven APVTS parameter system (shared across plugins).
//
// A FIXED, named union of the engine-supported controls a library actually uses,
// DISCOVERED from its manifest (so each plugin gets its params with no per-plugin
// param code). DAW automation lanes are stable and named. Float params are stored
// NORMALISED (0..1); each mode maps them through its own control's min..max range.
//
// The params are the single source of truth: the processor applies them to the
// engine every block (so automation works with the editor closed), and the editor
// only drives/reflects them. The binding->engine vocabulary lives here.
//
// To support a new engine parameter, add it to the registry in the .cpp (and make
// the engine actually apply it) — layout/apply/editor all flow from the registry.

#include <juce_audio_processors/juce_audio_processors.h>
#include <model/Manifest.h>
#include <SamplerEngine.h>
#include <atomic>
#include <cstdint>

namespace dm::params
{

// Always-present params (not derived from manifest controls).
namespace id
{
    inline constexpr const char* mode           = "mode";
    inline constexpr const char* pitchBendRange = "pitchBendRange";
    inline constexpr const char* chordOrder     = "chordOrder";   // first dropdown menu
    inline constexpr const char* masterOutput   = "masterOutput"; // user master output fader (dB)
}

/** Build the APVTS layout for a whole library (auto-union of every mode's
    engine-supported controls/buttons/menus, + mode + pitch-bend range). */
juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const PresetLibrary& library);

/** Apply the current parameter values to the engine for `mode`. Idempotent; call
    every block so a mode switch (which resets engine overrides) is re-honoured.
    `buttonClickSeq` (per-tab-button-index click counters, or nullptr) orders the button
    application by recency: buttons are applied oldest-click-first so that among any set
    of buttons targeting the SAME effect (a radio group, e.g. Strykebrett's ensemble
    O/Acc/Solo/Organ) the most recently clicked one wins — and clicking an unrelated
    button never disturbs that. Never-clicked buttons keep index order. */
void applyToEngine (SamplerEngine& engine, const Mode& mode,
                    juce::AudioProcessorValueTreeState& apvts,
                    const std::atomic<std::uint32_t>* buttonClickSeq = nullptr);

/** Map incoming MIDI CC (mod wheel etc.) to params per the mode's CC bindings. */
void applyCcToParams (const juce::MidiBuffer& midi, const Mode& mode,
                      juce::AudioProcessorValueTreeState& apvts);

/** Apply note key-switches (e.g. low keys selecting the chord-order menu). */
void applyNoteSwitches (const juce::MidiBuffer& midi, const Mode& mode,
                        juce::AudioProcessorValueTreeState& apvts);

/** The float param ids a control's bindings drive (for the editor to write/reflect). */
juce::StringArray controlParamIds (const Control& c);

/** The param id a button drives — one per button index, an Int 0..numStates-1
    (supports multi-state selectors, not just on/off). For the editor. */
juce::String buttonParamId (int buttonIndex);

} // namespace dm::params
