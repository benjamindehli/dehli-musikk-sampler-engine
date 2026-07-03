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
    juce::String type;            // lowpass | gain | convolution
    bool enabled = true;

    std::optional<double> frequency;     // lowpass
    std::optional<double> resonance;     // lowpass
    std::optional<double> gain;          // gain (from "level")
    std::optional<double> drive;
    std::optional<double> mix;
    std::optional<double> wet;           // convolution (from "wetLevel")
    std::optional<double> outputLevel;
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
    std::optional<double> decay;
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
    Rect rect;
    juce::String style;          // image | ...
    std::optional<int> value;
    juce::Array<ButtonState> states;
};

struct UiImage
{
    Rect rect;
    juce::String image;          // asset id
    juce::String aspectRatioMode;
    std::optional<int> controlIndex;   // document-order UI index; PATH bindings
                                       // target lights by this (e.g. button → light)
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
    Rect rect;
    int  value = 1;                 // selected option (1-based, as authored)
    juce::Array<MenuOption> options;

    // Optional styling (DecentSampler menu attrs). Empty = renderer default
    // (transparent background, left-aligned) — keeps unstyled menus as-is.
    juce::String textColor;         // ARGB hex
    juce::String backgroundColor;   // ARGB hex
    juce::String hAlign;            // left | center | right
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
struct Mode
{
    juce::String name;
    AmpEnvelope amp;
    juce::Array<Tag> tags;
    juce::Array<Group> groups;
    juce::Array<Effect> effects;
    juce::Array<NoteSequence> sequences;
    juce::Array<SequenceTrigger> sequenceTriggers;
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
    juce::Array<Mode> modes;
};

/** Schema version this engine build understands. The loader rejects manifests
    whose top-level "schema" exceeds this. */
inline constexpr int kManifestSchemaVersion = 1;

} // namespace dm
