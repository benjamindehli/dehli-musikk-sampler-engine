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
    inline constexpr const char* pitchBendUp    = "pitchBendUp";     // semitones, wheel above centre
    inline constexpr const char* pitchBendDown  = "pitchBendDown";   // semitones, wheel below centre
    inline constexpr const char* chordOrder     = "chordOrder";   // first dropdown menu
    inline constexpr const char* masterOutput   = "masterOutput"; // user master output fader (dB)
    inline constexpr const char* pitchDrift     = "pitchDrift";   // global pitch-drift wheel (0..1)
    inline constexpr const char* volumeDrift    = "volumeDrift";  // global volume-drift wheel (0..1)
    inline constexpr const char* skipMuted      = "skipMuted";    // skip triggering silent groups (bool)
    inline constexpr const char* maxPolyphony   = "maxPolyphony"; // voice-cap choice (see kPolyphonyChoices)
    inline constexpr const char* seqTempoSync   = "seqTempoSync"; // sequencer: free (off) / 16th steps at BPM (on)
    inline constexpr const char* seqSyncDaw     = "seqSyncDaw";   // synced BPM follows the host (DAW builds only)
    inline constexpr const char* seqBpm         = "seqBpm";       // manual BPM when not following the host
    inline constexpr const char* seqNoteValue   = "seqNoteValue"; // synced step length (see kNoteValue*)
    inline constexpr const char* masterTune     = "masterTune";   // master tuning in cents
    inline constexpr const char* velocityCurve  = "velocityCurve"; // 0 soft · 1 linear · 2 hard
}

// Voice-cap choices for the maxPolyphony parameter (choice index → voice count).
// The engine clamps to its compile-time pool, so the last entry = "engine maximum".
inline constexpr int kPolyphonyChoices[] = { 8, 16, 32, 64, 128 };
inline constexpr int kNumPolyphonyChoices = 5;

// Tempo-synced sequencer step lengths (choice index → label / beats). Triplet =
// 2/3 of the straight value, dotted = 1.5x.
inline constexpr const char* kNoteValueLabels[] = {
    "1/4", "1/4 triplet", "1/4 dotted",
    "1/8", "1/8 triplet", "1/8 dotted",
    "1/16", "1/16 triplet", "1/16 dotted",
    "1/32", "1/32 triplet", "1/32 dotted" };
inline constexpr double kNoteValueBeats[] = {
    1.0, 2.0 / 3.0, 1.5,
    0.5, 1.0 / 3.0, 0.75,
    0.25, 1.0 / 6.0, 0.375,
    0.125, 1.0 / 12.0, 0.1875 };
inline constexpr int kNumNoteValues = 12;
inline constexpr int kDefaultNoteValue = 6;   // 1/16

/** Build the APVTS layout for a whole library (auto-union of every mode's
    engine-supported controls/buttons/menus, + mode + pitch-bend range). */
juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const PresetLibrary& library);

// The per-block param→engine application lives in CompiledMode (CompiledMode.h):
// each mode's bindings are compiled ONCE at load into an allocation-free plan the
// audio thread applies every block. (The old applyToEngine/applyCcToParams/
// applyNoteSwitches re-did string lookups, table parsing and tag scans per block.)

/** Shared cap for per-button bookkeeping (click-sequence arrays, radio ordering). */
inline constexpr int kMaxUiButtons = 64;

/** The float param ids a control's bindings drive (for the editor to write/reflect). */
juce::StringArray controlParamIds (const Control& c);

/** The param id a button drives — an Int 0..numStates-1 (supports multi-state
    selectors, not just on/off). Keyed by the button's stable manifest id when
    present — a hand-authored manifest can reorder buttons without shifting saved
    sessions/automation — falling back to the positional btn_<index>. Converter-
    generated ids are btn_<i>, so existing saved sessions resolve identically. */
juce::String buttonParamId (const Button& button, int fallbackIndex);

} // namespace dm::params
