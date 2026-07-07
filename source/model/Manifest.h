#pragma once

// dehli-musikk-sampler-engine — native preset manifest, data model.
//
// This is the engine's OWN format (a JSON manifest), NOT DecentSampler `.dspreset`.
// ds-plugin-converter translates `.dspreset` libraries into manifests of this
// shape; the engine only ever loads this. See ../../CLAUDE.md and ../../../PLAN.md.
//
// The structs below mirror the manifest 1:1 and are produced by ManifestLoader.
// They are plain data (no behaviour); the audio/UI/params layers consume them in
// later milestones. Optional fields use std::optional so "absent" is distinct from
// "zero" — the loader leaves them empty when the manifest omits the key.

#include <juce_core/juce_core.h>
#include <optional>

namespace dm
{

/** Pixel rectangle for a UI node. */
struct Rect
{
    int x = 0, y = 0, width = 0, height = 0;
};

// ---------------------------------------------------------------------------
// Bindings — how a UI control / LFO / sequence drives an engine parameter.
// A wide, open vocabulary in DecentSampler; we keep the common fields typed and
// preserve the full original object in `raw` so nothing is ever lost.
// ---------------------------------------------------------------------------
struct Binding
{
    juce::String type;        // amp | effect | control | general | note_binding | note_sequence | ...
    juce::String level;       // instrument | group | tag | ui | midi | <numeric group/tag index>
    juce::String targetId;    // id of the element this binding targets (effect/group/modulator/
                              // control). Replaces the positional *Index selectors below; the
                              // index fields remain as a transition fallback until fully migrated.
    juce::String parameter;   // FX_FILTER_FREQUENCY, ENV_RELEASE, SEQ_INDEX, PATH, ...
    juce::String translation; // fixed_value | linear | table (empty = none)
    juce::String modBehavior; // set (LFO bindings)
    juce::String identifier;  // tag name for level="tag" bindings (TAG_VOLUME/TAG_ENABLED)
    juce::String translationTable; // "in,out;in,out;..." lookup (translation="table")
    bool translationReversed = false; // reverse the table input (max↔min)

    std::optional<double> factor;     // scales the source value
    std::optional<double> modAmount;
    std::optional<double> translationOutputMin;
    std::optional<double> translationOutputMax;

    // Index selectors (which effect/control/group/note/sequence this targets).
    std::optional<int> effectIndex;
    std::optional<int> controlIndex;
    std::optional<int> groupIndex;
    std::optional<int> noteIndex;
    std::optional<int> bindingIndex;
    std::optional<int> seqIndex;
    std::optional<int> position;

    // fixed_value payload — polymorphic in the source (bool / number / path string),
    // so kept as a var.
    juce::var translationValue;

    // The complete original binding object (all keys, including the long tail of
    // seq* flags we don't yet interpret). Lets M4+/M6 read anything without a
    // schema bump.
    juce::var raw;
};

// ---------------------------------------------------------------------------
// Amp envelope (mode-level defaults; groups may override a subset).
// ---------------------------------------------------------------------------
struct AmpEnvelope
{
    double attack  = 0.0;
    double decay   = 0.0;
    double sustain = 1.0;
    double release = 0.0;
    double volume  = 1.0;
    double velTrack = 0.0;     // ampVelTrack
    bool   enabled  = true;    // ampEnvEnabled

    std::optional<double> attackCurve;
    std::optional<double> decayCurve;
    std::optional<double> releaseCurve;
};

// ---------------------------------------------------------------------------
// Sample — one audio file mapped to a note (or note range).
// ---------------------------------------------------------------------------
struct SampleLoop
{
    bool enabled = false;
    std::optional<int>    start;     // frames
    std::optional<int>    end;       // frames
    std::optional<int>    crossfade; // frames
};

struct Sample
{
    juce::String source;        // asset id, e.g. "flac:Bass_0C"

    int loNote = 0, hiNote = 127, rootNote = 60;

    std::optional<int>    lengthFrames;
    std::optional<double> sampleRate;
    bool                  pitchKeyTrack = false;
    std::optional<double> pitchDrift;   // per-sample pitch-drift depth (fractional DS
                                        // pitchKeyTrack, 0<x<1); each voice drifts by this much

    std::optional<int>    start;     // playback start offset (frames)
    std::optional<int>    end;       // playback end offset (frames); absent/0 = to end
    std::optional<double> volume;    // per-sample gain
    std::optional<int>    seqPosition;   // round-robin slot (1-based, per group seqMode)
    std::optional<bool>   ampEnvEnabled; // per-sample override

    // Sustain-pedal (CC64) gate: when present (and loNote=-1), the sample is NOT
    // note-triggered — it fires when CC64 transitions into [onLoCC64, onHiCC64]
    // (e.g. damper-lift noise on pedal down, damper-drop on pedal up).
    std::optional<int>    onLoCC64;
    std::optional<int>    onHiCC64;

    SampleLoop loop;
};

// ---------------------------------------------------------------------------
// Effect — an insert effect (lowpass / gain / convolution). Used both for the
// instrument FX chain (Mode.effects) and per-group chains (Group.effects). All
// effect-specific fields are optional; only those relevant to `type` are populated.
// ---------------------------------------------------------------------------
struct Effect
{
    juce::String id;              // stable unique id (binding targets resolve to this)
    juce::String type;            // lowpass | gain | convolution
    bool enabled = true;

    std::optional<double> frequency;     // lowpass
    std::optional<double> resonance;     // lowpass
    std::optional<double> gain;          // gain (from "level")
    std::optional<double> drive;
    std::optional<double> mix;
    std::optional<double> wet;           // convolution (from "wetLevel")
    std::optional<double> outputLevel;
    std::optional<double> rate;          // chorus modRate (Hz)
    std::optional<double> depth;         // chorus modDepth (0..1)
    std::optional<double> feedback;      // chorus feedback (-1..1)
    juce::String ir;                     // convolution IR asset id (from "irFile")
    bool normalizeIr = true;             // normalise the IR's energy (off = use it as recorded, like DS)

    juce::var raw;                       // full original object
};

// ---------------------------------------------------------------------------
// Group — a set of samples sharing voicing, velocity range, round-robin, etc.
// ---------------------------------------------------------------------------
struct VelocityRange
{
    int lo = 0, hi = 127;
};

struct RoundRobin
{
    juce::String mode;        // e.g. "random", "round_robin"
    std::optional<int> length;
};

struct Silencing
{
    juce::String mode;                 // e.g. "normal"
    juce::StringArray byTags;          // silencedByTags
};

struct Group
{
    juce::String uid;                  // optional stable id
    juce::StringArray tags;
    juce::String trigger;              // attack (default) | release
    juce::String loopCrossfadeMode;    // e.g. "linear"

    std::optional<VelocityRange> velocity;   // loVel/hiVel
    std::optional<RoundRobin>    roundRobin;  // seqMode/seqLength
    std::optional<Silencing>     silencing;

    // Group-level amp / playback overrides (only present when the group sets them).
    std::optional<double> attack;
    std::optional<double> decay;
    std::optional<double> sustain;
    std::optional<double> release;
    std::optional<double> volume;
    std::optional<double> velTrack;
    std::optional<bool>   ampEnvEnabled;
    std::optional<bool>   pitchKeyTrack;

    // Per-group insert effects (lowpass/gain), applied to this group's voices before
    // mixing — e.g. an organ's per-stop swell filter + loudness. Bindings target
    // these by (groupIndex, effectIndex). Empty for most libraries.
    juce::Array<Effect> effects;

    juce::Array<Sample> samples;
};

// ---------------------------------------------------------------------------
// LFO / modulator.
// ---------------------------------------------------------------------------
struct Lfo
{
    juce::String id;              // stable unique id (MOD_AMOUNT/FREQUENCY targets resolve to this)
    juce::String shape;           // sine | ...
    double frequency = 0.0;
    double modAmount = 0.0;
    juce::Array<Binding> bindings;
};

// ---------------------------------------------------------------------------
// Note sequence (auto-strum). Distinct from a group's round-robin.
// ---------------------------------------------------------------------------
struct SequenceNote
{
    int    position = 0;
    int    note = 60;
    double velocity = 1.0;
    double length = 1.0;
    bool   enabled = true;
    bool   swallowNotes = false;
};

struct NoteSequence
{
    juce::String name;
    std::optional<int>    length;
    std::optional<double> rate;
    juce::Array<SequenceNote> notes;
};

/** Binds a played key to a sequence: pressing `note` fires `sequence` (index into
    Mode.sequences) as timed note events. The engine's NoteSequencer interprets
    these; "auto-strum" is just this with one trigger per chord key. */
struct SequenceTrigger
{
    int    note = 60;            // the key that starts the sequence
    int    sequence = 0;         // index into Mode.sequences
    int    transpose = 0;        // semitones added to the fired notes
    double rate = 10.0;          // playback rate (steps/second, free-running)
    bool   loop = false;         // false = one-shot strum, true = repeat
    bool   trackVelocity = true; // scale fired velocity by the key's velocity
    bool   swallow = true;       // suppress the trigger key's own note
};

// ---------------------------------------------------------------------------
// UI tree.
// ---------------------------------------------------------------------------
struct CustomSkin
{
    juce::String image;          // asset id
    std::optional<int> numFrames;
    juce::String orientation;    // vertical | horizontal
};

struct Control
{
    juce::String id;             // stable unique id (VISIBLE/OPACITY/VALUE/CC targets resolve to this)
    Rect rect;
    juce::String label;          // parameterName
    juce::String valueType;      // float | percent
    std::optional<double> min;
    std::optional<double> max;
    std::optional<double> value;
    juce::String textColor;      // ARGB hex
    juce::String style;          // custom_skin_vertical_drag | image | ...
    std::optional<CustomSkin> skin;
    std::optional<double> mouseDragSensitivity;
    juce::Array<Binding> bindings;
    std::optional<int> controlIndex;   // document-order UI index (VISIBLE/OPACITY binding target)
    bool visible = true;               // default visibility (DecentSampler `visible` attr)
};

struct ButtonState
{
    juce::String name;
    juce::String mainImage;      // asset ids
    juce::String hoverImage;
    juce::String clickImage;
    juce::Array<Binding> bindings;
};

struct Button
{
    juce::String id;             // stable unique id (VISIBLE/OPACITY/VALUE/PATH targets resolve to this)
    Rect rect;
    juce::String style;          // image | ...
    std::optional<int> value;
    juce::Array<ButtonState> states;
    std::optional<int> controlIndex;   // document-order UI index (VISIBLE/OPACITY binding target)
    bool visible = true;               // default visibility (DecentSampler `visible` attr)
};

struct UiImage
{
    juce::String id;             // stable unique id (VISIBLE/OPACITY/PATH targets resolve to this)
    Rect rect;
    juce::String image;          // asset id
    juce::String aspectRatioMode;
    std::optional<int> controlIndex;   // document-order UI index; PATH bindings
                                       // target lights by this (e.g. button → light)
    bool visible = true;               // default visibility (DecentSampler `visible` attr)
};

/** A dropdown option. `seqIndex` is the SEQ_INDEX it selects (Omni-84's
    chord-ordering menu maps each option to a 0/84/168/252 sequence-block offset). */
struct MenuOption
{
    juce::String name;
    int seqIndex = 0;
    // Effect bindings applied when this option is selected (e.g. an amp/cabinet
    // selector: ENABLED / FX_DRIVE / FX_OUTPUT_LEVEL / FX_MIX / FX_IR_FILE).
    juce::Array<Binding> bindings;
};

struct Menu
{
    juce::String id;                // stable unique id (VISIBLE/OPACITY/VALUE targets resolve to this)
    Rect rect;
    int  value = 1;                 // selected option (1-based, as authored)
    juce::Array<MenuOption> options;

    // Optional styling (DecentSampler menu attrs). Empty = renderer default
    // (transparent background, left-aligned) — keeps unstyled menus as-is.
    juce::String textColor;         // ARGB hex
    juce::String backgroundColor;   // ARGB hex
    juce::String hAlign;            // left | center | right

    std::optional<int> controlIndex;   // document-order UI index (VISIBLE/OPACITY binding target)
    bool visible = true;               // default visibility (DecentSampler `visible` attr)
};

struct Tab
{
    juce::String name;
    juce::Array<Control> controls;
    juce::Array<Button>  buttons;
    juce::Array<UiImage> images;
    juce::Array<Menu>    menus;
};

struct KeyboardColor
{
    int loNote = 0, hiNote = 127;
    juce::String color;          // ARGB hex
};

// When button `fromButton` is set to state `fromState`, the editor also forces button
// `toButton` to state `toState`. Used e.g. so turning Stereo on auto-enables Double Track.
struct ButtonLink
{
    // Positional endpoints (converter output; tab-0 button indices) …
    int fromButton = -1, fromState = -1;
    int toButton   = -1, toState   = -1;
    // … or id-based endpoints (preferred for hand-authored manifests — they survive
    // button reordering). Non-empty ids win; the editor resolves them to indices at use.
    juce::String fromId, toId;
};

struct Ui
{
    juce::String background;     // bg image asset id
    int width = 0, height = 0;
    int cropTop = 0;             // design-px trimmed off the TOP: shrinks the height,
                                 // shifts every element up, and shows only the lower
                                 // part of the background (reclaims dead header space).
    juce::String layoutMode;     // relative | ...
    juce::String bgMode;         // top_left | ...
    juce::Array<Tab> tabs;
    juce::Array<KeyboardColor> keyboardColors;
    // Optional global per-key-type tint (ARGB hex; alpha = strength). Overlaid on ALL
    // white / black keys respectively — independent of keyboardColors' note-range zones.
    // e.g. whiteKeyTint "30ffcc00" = subtle yellow on white keys only. Empty = none.
    juce::String whiteKeyTint;
    juce::String blackKeyTint;
    juce::Array<ButtonLink> buttonLinks;   // "when button X → state S, set button Y → state T"
};

// ---------------------------------------------------------------------------
// Tags & top-level structures.
// ---------------------------------------------------------------------------
struct Tag
{
    juce::String name;
    std::optional<int> polyphony;
};

/** A MIDI CC → control mapping (DecentSampler `<midi><cc>`). The converter resolves
    the target control's primary engine parameter and pre-computes the normalised
    value range, so the host can map CC → the matching parameter directly. */
struct CcBinding
{
    int cc = 1;                     // controller number (1 = mod wheel)
    juce::String parameter;         // engine binding parameter the control drives (e.g. SEQ_PLAYBACK_RATE)
    std::optional<int> groupIndex;
    juce::String       targetId;     // id of the specific target control (preferred over controlIndex)
    std::optional<int> controlIndex; // the SPECIFIC target control (document index); when set,
                                     // drive only that control, not every one sharing `parameter`
    double normMin = 0.0;           // normalised parameter value at CC 0
    double normMax = 1.0;           // normalised parameter value at CC 127
};

/** A MIDI note that selects a dropdown-menu option (DecentSampler note→control
    "VALUE" key-switch on a menu, e.g. low keys choosing the chord order). */
struct MenuKeySwitch
{
    int note = 0;
    int option = 0;                 // 0-based menu option to select
};

/** A "preset" in DecentSampler terms = one selectable mode of the plugin. */
// Omnichord-style strum key ("select+strum" sequencing). When a mode carries any of
// these, its sequenceTriggers become chord SELECTORS (pressing one only chooses the
// sequence) and each strum key FIRES the selected sequence, offset by seqOffset into
// the sequence list (e.g. +84 per note-order block on Omni-84). The chord-order menu
// offset is ignored in this mode. Changing the selection while strummed notes still
// ring morphs them to the new chord (same position in time, small crossfade).
struct StrumKey
{
    int note = -1;                 // MIDI key that strums
    int seqOffset = 0;             // added to the selected trigger's sequence index
    std::optional<double> rate;    // steps/sec override for this key (else the trigger's)
};

struct Mode
{
    juce::String name;
    AmpEnvelope amp;
    juce::Array<Tag> tags;
    juce::Array<Group> groups;
    juce::Array<Effect> effects;
    juce::Array<NoteSequence> sequences;
    juce::Array<SequenceTrigger> sequenceTriggers;
    juce::Array<StrumKey> strumKeys;   // non-empty → select+strum mode (see StrumKey)
    juce::Array<Lfo> modulators;
    juce::Array<CcBinding> ccBindings;
    juce::Array<MenuKeySwitch> menuKeySwitches;
    Ui ui;
};

/** The whole manifest: a library of modes for one plugin. */
struct PresetLibrary
{
    int schema = 0;              // manifest schema version
    juce::String format;         // "dmse-manifest"
    juce::String library;        // human name, e.g. "Omni-84"
    double gainDb = 0.0;         // pre-FX level trim applied to all modes (matches DS signal level)
    bool polySaveDefault = true; // default for the Poly-save toggle (skip silent groups at note-on).
                                 // false for libraries whose controls BLEND muted groups in mid-note
                                 // (e.g. Elektrisk's mod-wheel Normal/Full sweep).
    juce::Array<Mode> modes;
};

/** Schema version this engine build understands. The loader rejects manifests
    whose top-level "schema" exceeds this. */
// Schema version POLICY: ADDITIVE fields (new optional keys) do NOT bump this —
// an older engine warns about the unknown keys (loader lint) and plays what it
// understands; embedded manifests are always regenerated with their plugin anyway.
// Bump ONLY for changes that alter the MEANING of existing fields (an old engine
// would misinterpret, not just miss, the data). Newer-schema manifests are
// rejected with a clear error; schema-0 (missing) loads with a warning.
inline constexpr int kManifestSchemaVersion = 1;

} // namespace dm
