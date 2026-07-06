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
    inline constexpr const char* pitchDrift     = "pitchDrift";   // global pitch-drift wheel (0..1)
    inline constexpr const char* volumeDrift    = "volumeDrift";  // global volume-drift wheel (0..1)
    inline constexpr const char* skipMuted      = "skipMuted";    // skip triggering silent groups (bool)
}

/** Build the APVTS layout for a whole library (auto-union of every mode's
    engine-supported controls/buttons/menus, + mode + pitch-bend range). */
juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const PresetLibrary& library);

// The per-block param→engine application lives in CompiledMode (CompiledMode.h):
// each mode's bindings are compiled ONCE at load into an allocation-free plan the
// audio thread applies every block. (The old applyToEngine/applyCcToParams/
// applyNoteSwitches re-did string lookups, table parsing and tag scans per block.)

/** The float param ids a control's bindings drive (for the editor to write/reflect). */
juce::StringArray controlParamIds (const Control& c);

/** The param id a button drives — one per button index, an Int 0..numStates-1
    (supports multi-state selectors, not just on/off). For the editor. */
juce::String buttonParamId (int buttonIndex);

} // namespace dm::params
