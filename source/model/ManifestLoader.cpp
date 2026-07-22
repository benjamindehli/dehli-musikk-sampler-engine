#include "ManifestLoader.h"

namespace dm
{

namespace
{
using juce::var;

// --- small, total accessors over a JSON object var -------------------------
// All return a sensible default when the key is absent or the var isn't an
// object, so parsing never throws on a missing optional field.

var get (const var& v, const char* key)            { return v.getProperty (key, var()); }

juce::String str (const var& v, const char* key, const juce::String& def = {})
{
    auto p = get (v, key);
    return p.isVoid() ? def : p.toString();
}

std::optional<double> optD (const var& v, const char* key)
{
    auto p = get (v, key);
    if (p.isVoid()) return std::nullopt;
    return (double) p;
}

std::optional<int> optI (const var& v, const char* key)
{
    auto p = get (v, key);
    if (p.isVoid()) return std::nullopt;
    return (int) p;
}

std::optional<bool> optB (const var& v, const char* key)
{
    auto p = get (v, key);
    if (p.isVoid()) return std::nullopt;
    return (bool) p;
}

double dbl  (const var& v, const char* key, double def) { auto p = get (v, key); return p.isVoid() ? def : (double) p; }
int    intg (const var& v, const char* key, int def)    { auto p = get (v, key); return p.isVoid() ? def : (int) p; }
bool   boolean (const var& v, const char* key, bool def){ auto p = get (v, key); return p.isVoid() ? def : (bool) p; }

// ---------------------------------------------------------------------------
// Lint: unknown-key detection. A hand-authored typo ("lenghtFrames") used to be
// silently ignored on load AND silently dropped on write — the field just
// vanished. Every node parser now reports keys it doesn't recognise as WARNINGS
// (never errors — newer manifests stay loadable by design, see the schema policy
// in Manifest.h). The loader is single-use per call; loadManifest installs the
// sink for its duration.
// ---------------------------------------------------------------------------
juce::StringArray* lintSink = nullptr;

void checkKeys (const var& v, const char* node, std::initializer_list<const char*> allowed)
{
    if (lintSink == nullptr)
        return;
    if (auto* o = v.getDynamicObject())
        for (const auto& p : o->getProperties())
        {
            const auto name = p.name.toString();
            if (name.startsWith ("$"))   // split-manifest directives ($use/$ref)
                continue;
            bool known = false;
            for (const auto* k : allowed)
                if (name == k) { known = true; break; }
            if (! known)
                lintSink->addIfNotAlreadyThere (
                    juce::String (node) + ": unknown key \"" + name + "\" — ignored (typo?)");
        }
}

struct ScopedLintSink
{
    explicit ScopedLintSink (juce::StringArray& sink) { lintSink = &sink; }
    ~ScopedLintSink() { lintSink = nullptr; }
};

juce::StringArray strArray (const var& v, const char* key)
{
    juce::StringArray out;
    if (auto* a = get (v, key).getArray())
        for (auto& e : *a)
            out.add (e.toString());
    return out;
}

// --- node parsers ----------------------------------------------------------

Binding parseBinding (const var& v)
{
    Binding b;
    checkKeys (v, "binding", { "type", "level", "targetId", "identifier", "translationTable",
                               "translationReversed", "parameter", "translation", "modBehavior",
                               "factor", "modAmount", "translationOutputMin", "translationOutputMax",
                               "effectIndex", "controlIndex", "groupIndex", "noteIndex",
                               "bindingIndex", "seqIndex", "position", "translationValue" });
    b.raw         = v;
    b.type        = str (v, "type");
    b.level       = str (v, "level");
    b.targetId    = str (v, "targetId");
    b.identifier  = str (v, "identifier");
    b.translationTable = str (v, "translationTable");
    b.translationReversed = boolean (v, "translationReversed", false);
    b.parameter   = str (v, "parameter");
    b.translation = str (v, "translation");
    b.modBehavior = str (v, "modBehavior");

    b.factor              = optD (v, "factor");
    b.modAmount           = optD (v, "modAmount");
    b.translationOutputMin = optD (v, "translationOutputMin");
    b.translationOutputMax = optD (v, "translationOutputMax");

    b.effectIndex  = optI (v, "effectIndex");
    b.controlIndex = optI (v, "controlIndex");
    b.groupIndex   = optI (v, "groupIndex");
    b.noteIndex    = optI (v, "noteIndex");
    b.bindingIndex = optI (v, "bindingIndex");
    b.seqIndex     = optI (v, "seqIndex");
    b.position     = optI (v, "position");

    b.translationValue = get (v, "translationValue");
    return b;
}

juce::Array<Binding> parseBindings (const var& parent)
{
    juce::Array<Binding> out;
    if (auto* a = get (parent, "bindings").getArray())
        for (auto& e : *a)
            out.add (parseBinding (e));
    return out;
}

Rect parseRect (const var& v)
{
    Rect r;
    checkKeys (v, "rect", { "x", "y", "width", "height" });
    r.x      = intg (v, "x", 0);
    r.y      = intg (v, "y", 0);
    r.width  = intg (v, "width", 0);
    r.height = intg (v, "height", 0);
    return r;
}

AmpEnvelope parseAmp (const var& v)
{
    AmpEnvelope a;
    checkKeys (v, "amp", { "attack", "decay", "sustain", "release", "volume", "velTrack",
                           "enabled", "attackCurve", "decayCurve", "releaseCurve" });
    a.attack   = dbl (v, "attack", 0.0);
    a.decay    = dbl (v, "decay", 0.0);
    a.sustain  = dbl (v, "sustain", 1.0);
    a.release  = dbl (v, "release", 0.0);
    a.volume   = dbl (v, "volume", 1.0);
    a.velTrack = dbl (v, "velTrack", 0.0);
    a.enabled  = boolean (v, "enabled", true);
    a.attackCurve  = optD (v, "attackCurve");
    a.decayCurve   = optD (v, "decayCurve");
    a.releaseCurve = optD (v, "releaseCurve");
    return a;
}

Sample parseSample (const var& v, ManifestParseResult& res, const juce::String& where)
{
    Sample s;
    checkKeys (v, "sample", { "source", "loNote", "hiNote", "rootNote", "lengthFrames",
                              "sampleRate", "pitchKeyTrack", "pitchDrift", "start", "end",
                              "volume", "seqPosition", "ampEnvEnabled", "onLoCC64", "onHiCC64",
                              "loop" });
    s.source = str (v, "source");
    if (s.source.isEmpty())
        res.errors.add (where + ": sample missing \"source\"");

    s.loNote   = intg (v, "loNote", 0);
    s.hiNote   = intg (v, "hiNote", 127);
    s.rootNote = intg (v, "rootNote", 60);

    s.lengthFrames  = optI (v, "lengthFrames");
    s.sampleRate    = optD (v, "sampleRate");
    s.pitchKeyTrack = boolean (v, "pitchKeyTrack", false);
    s.pitchDrift    = optD (v, "pitchDrift");

    s.start         = optI (v, "start");
    s.end           = optI (v, "end");
    s.volume        = optD (v, "volume");
    s.seqPosition   = optI (v, "seqPosition");
    s.ampEnvEnabled = optB (v, "ampEnvEnabled");
    s.onLoCC64      = optI (v, "onLoCC64");
    s.onHiCC64      = optI (v, "onHiCC64");

    auto loop = get (v, "loop");
    if (! loop.isVoid())
    {
        checkKeys (loop, "sample.loop", { "enabled", "start", "end", "crossfade" });
        s.loop.enabled   = boolean (loop, "enabled", false);
        s.loop.start     = optI (loop, "start");
        s.loop.end       = optI (loop, "end");
        s.loop.crossfade = optI (loop, "crossfade");
    }
    return s;
}

Effect parseEffect (const var& v);   // defined below

Group parseGroup (const var& v, ManifestParseResult& res, const juce::String& where)
{
    Group g;
    checkKeys (v, "group", { "uid", "tags", "trigger", "loopCrossfadeMode", "velocity",
                             "roundRobin", "silencing", "attack", "decay", "sustain", "release",
                             "volume", "velTrack", "ampEnvEnabled", "pitchKeyTrack",
                             "effects", "samples" });
    g.uid               = str (v, "uid");
    g.tags              = strArray (v, "tags");
    g.trigger           = str (v, "trigger");
    g.loopCrossfadeMode = str (v, "loopCrossfadeMode");

    if (auto vel = get (v, "velocity"); ! vel.isVoid())
    {
        checkKeys (vel, "group.velocity", { "lo", "hi" });
        VelocityRange vr;
        vr.lo = intg (vel, "lo", 0);
        vr.hi = intg (vel, "hi", 127);
        g.velocity = vr;
    }

    if (auto rr = get (v, "roundRobin"); ! rr.isVoid())
    {
        checkKeys (rr, "group.roundRobin", { "mode", "length" });
        RoundRobin r;
        r.mode   = str (rr, "mode");
        r.length = optI (rr, "length");
        g.roundRobin = r;
    }

    if (auto sil = get (v, "silencing"); ! sil.isVoid())
    {
        checkKeys (sil, "group.silencing", { "mode", "byTags" });
        Silencing s;
        s.mode   = str (sil, "mode");
        s.byTags = strArray (sil, "byTags");
        g.silencing = s;
    }

    g.attack        = optD (v, "attack");
    g.decay         = optD (v, "decay");
    g.sustain       = optD (v, "sustain");
    g.release       = optD (v, "release");
    g.volume        = optD (v, "volume");
    g.velTrack      = optD (v, "velTrack");
    g.ampEnvEnabled = optB (v, "ampEnvEnabled");
    g.pitchKeyTrack = optB (v, "pitchKeyTrack");

    if (auto* a = get (v, "effects").getArray())
        for (auto& e : *a)
            g.effects.add (parseEffect (e));

    if (auto* a = get (v, "samples").getArray())
    {
        int i = 0;
        for (auto& e : *a)
            g.samples.add (parseSample (e, res, where + " sample[" + juce::String (i++) + "]"));
    }
    else
    {
        res.warnings.add (where + ": group has no \"samples\" array");
    }
    return g;
}

Effect parseEffect (const var& v)
{
    Effect e;
    checkKeys (v, "effect", { "id", "type", "enabled", "frequency", "resonance", "gain",
                              "drive", "mix", "wet", "outputLevel", "rate", "depth",
                              "feedback", "ir", "normalizeIr" });
    e.raw     = v;
    e.id      = str (v, "id");
    e.type    = str (v, "type");
    e.enabled = boolean (v, "enabled", true);

    e.frequency   = optD (v, "frequency");
    e.resonance   = optD (v, "resonance");
    e.gain        = optD (v, "gain");
    e.drive       = optD (v, "drive");
    e.mix         = optD (v, "mix");
    e.wet         = optD (v, "wet");
    e.outputLevel = optD (v, "outputLevel");
    e.rate        = optD (v, "rate");
    e.depth       = optD (v, "depth");
    e.feedback    = optD (v, "feedback");
    e.ir          = str (v, "ir");
    e.normalizeIr = boolean (v, "normalizeIr", true);
    return e;
}

Lfo parseLfo (const var& v)
{
    Lfo l;
    checkKeys (v, "modulator", { "id", "shape", "frequency", "modAmount", "bindings" });
    l.id        = str (v, "id");
    l.shape     = str (v, "shape");
    l.frequency = dbl (v, "frequency", 0.0);
    l.modAmount = dbl (v, "modAmount", 0.0);
    l.bindings  = parseBindings (v);
    return l;
}

NoteSequence parseSequence (const var& v)
{
    NoteSequence seq;
    checkKeys (v, "sequence", { "name", "length", "rate", "notes" });
    seq.name   = str (v, "name");
    seq.length = optI (v, "length");
    seq.rate   = optD (v, "rate");

    if (auto* a = get (v, "notes").getArray())
    {
        for (auto& n : *a)
        {
            SequenceNote note;
            checkKeys (n, "sequence.note", { "position", "note", "velocity", "length",
                                             "enabled", "swallowNotes" });
            note.position     = intg (n, "position", 0);
            note.note         = intg (n, "note", 60);
            note.velocity     = dbl (n, "velocity", 1.0);
            note.length       = dbl (n, "length", 1.0);
            note.enabled      = boolean (n, "enabled", true);
            note.swallowNotes = boolean (n, "swallowNotes", false);
            seq.notes.add (note);
        }
    }
    return seq;
}

std::optional<CustomSkin> parseSkin (const var& v)
{
    auto s = get (v, "skin");
    if (s.isVoid())
        return std::nullopt;

    CustomSkin skin;
    checkKeys (s, "control.skin", { "image", "numFrames", "orientation" });
    skin.image       = str (s, "image");
    skin.numFrames   = optI (s, "numFrames");
    skin.orientation = str (s, "orientation");
    return skin;
}

Control parseControl (const var& v)
{
    Control c;
    checkKeys (v, "control", { "id", "rect", "label", "valueType", "min", "max", "value",
                               "textColor", "style", "skin", "mouseDragSensitivity",
                               "controlIndex", "visible", "bindings" });
    c.id        = str (v, "id");
    c.rect      = parseRect (get (v, "rect"));
    c.label     = str (v, "label");
    c.valueType = str (v, "valueType");
    c.min       = optD (v, "min");
    c.max       = optD (v, "max");
    c.value     = optD (v, "value");
    c.textColor = str (v, "textColor");
    c.style     = str (v, "style");
    c.skin      = parseSkin (v);
    c.mouseDragSensitivity = optD (v, "mouseDragSensitivity");
    c.controlIndex = optI (v, "controlIndex");
    c.visible   = boolean (v, "visible", true);
    c.bindings  = parseBindings (v);
    return c;
}

Button parseButton (const var& v)
{
    Button b;
    checkKeys (v, "button", { "id", "rect", "style", "value", "controlIndex", "visible",
                              "states" });
    b.id    = str (v, "id");
    b.rect  = parseRect (get (v, "rect"));
    b.style = str (v, "style");
    b.value = optI (v, "value");
    b.controlIndex = optI (v, "controlIndex");
    b.visible = boolean (v, "visible", true);

    if (auto* a = get (v, "states").getArray())
    {
        for (auto& s : *a)
        {
            ButtonState st;
            checkKeys (s, "button.state", { "name", "mainImage", "hoverImage", "clickImage",
                                            "bindings" });
            st.name       = str (s, "name");
            st.mainImage  = str (s, "mainImage");
            st.hoverImage = str (s, "hoverImage");
            st.clickImage = str (s, "clickImage");
            st.bindings   = parseBindings (s);
            b.states.add (st);
        }
    }
    return b;
}

UiImage parseImage (const var& v)
{
    UiImage img;
    checkKeys (v, "image", { "id", "rect", "image", "aspectRatioMode", "controlIndex",
                             "visible" });
    img.id              = str (v, "id");
    img.rect            = parseRect (get (v, "rect"));
    img.image           = str (v, "image");
    img.aspectRatioMode = str (v, "aspectRatioMode");
    img.controlIndex    = optI (v, "controlIndex");
    img.visible         = boolean (v, "visible", true);
    return img;
}

Ui parseUi (const var& v)
{
    Ui ui;
    checkKeys (v, "ui", { "background", "overlay", "width", "height", "cropTop", "layoutMode", "bgMode",
                          "whiteKeyTint", "blackKeyTint", "tabs", "keyboard", "buttonLinks",
                          "strumSpeedReadout" });
    ui.background = str (v, "background");
    ui.overlay    = str (v, "overlay");
    ui.width      = intg (v, "width", 0);
    ui.height     = intg (v, "height", 0);
    ui.cropTop    = intg (v, "cropTop", 0);
    ui.layoutMode = str (v, "layoutMode");
    ui.bgMode     = str (v, "bgMode");
    ui.whiteKeyTint = str (v, "whiteKeyTint");
    ui.blackKeyTint = str (v, "blackKeyTint");
    if (const auto sr = get (v, "strumSpeedReadout"); sr.isObject())
        ui.strumSpeedReadout = parseRect (sr);

    if (auto* tabs = get (v, "tabs").getArray())
    {
        for (auto& t : *tabs)
        {
            Tab tab;
            checkKeys (t, "ui.tab", { "name", "controls", "buttons", "images", "menus" });
            tab.name = str (t, "name");
            if (auto* a = get (t, "controls").getArray()) for (auto& e : *a) tab.controls.add (parseControl (e));
            if (auto* a = get (t, "buttons").getArray())  for (auto& e : *a) tab.buttons.add  (parseButton (e));
            if (auto* a = get (t, "images").getArray())   for (auto& e : *a) tab.images.add   (parseImage (e));
            if (auto* a = get (t, "menus").getArray())
                for (auto& e : *a)
                {
                    Menu menu;
                    checkKeys (e, "menu", { "id", "rect", "value", "textColor",
                                            "backgroundColor", "hAlign", "controlIndex",
                                            "visible", "options" });
                    menu.id    = str (e, "id");
                    menu.rect  = parseRect (get (e, "rect"));
                    menu.value = intg (e, "value", 1);
                    menu.textColor       = str (e, "textColor");
                    menu.backgroundColor = str (e, "backgroundColor");
                    menu.hAlign          = str (e, "hAlign");
                    menu.controlIndex    = optI (e, "controlIndex");
                    menu.visible         = boolean (e, "visible", true);
                    if (auto* opts = get (e, "options").getArray())
                        for (auto& o : *opts)
                        {
                            MenuOption mo;
                            checkKeys (o, "menu.option", { "name", "seqIndex", "bindings" });
                            mo.name     = str (o, "name");
                            mo.seqIndex = intg (o, "seqIndex", 0);
                            mo.bindings = parseBindings (o);
                            menu.options.add (mo);
                        }
                    tab.menus.add (menu);
                }
            ui.tabs.add (tab);
        }
    }

    auto kb = get (v, "keyboard");
    if (auto* colors = get (kb, "colors").getArray())
    {
        for (auto& c : *colors)
        {
            KeyboardColor kc;
            checkKeys (c, "keyboard.color", { "loNote", "hiNote", "color" });
            kc.loNote = intg (c, "loNote", 0);
            kc.hiNote = intg (c, "hiNote", 127);
            kc.color  = str (c, "color");
            ui.keyboardColors.add (kc);
        }
    }
    if (auto* labels = get (kb, "labels").getArray())
    {
        for (auto& l : *labels)
        {
            KeyboardLabel kl;
            checkKeys (l, "keyboard.label", { "loNote", "hiNote", "text" });
            kl.loNote = intg (l, "loNote", 0);
            kl.hiNote = intg (l, "hiNote", 127);
            kl.text   = str (l, "text");
            ui.keyboardLabels.add (kl);
        }
    }

    if (auto* links = get (v, "buttonLinks").getArray())
    {
        for (auto& l : *links)
        {
            ButtonLink bl;
            checkKeys (l, "buttonLink", { "fromButton", "fromState", "toButton", "toState",
                                          "fromId", "toId" });
            bl.fromButton = intg (l, "fromButton", -1);
            bl.fromState  = intg (l, "fromState", -1);
            bl.toButton   = intg (l, "toButton", -1);
            bl.toState    = intg (l, "toState", -1);
            bl.fromId     = str (l, "fromId");   // id endpoints (hand-authored manifests)
            bl.toId       = str (l, "toId");
            ui.buttonLinks.add (bl);
        }
    }
    return ui;
}

Mode parseMode (const var& v, ManifestParseResult& res, int index)
{
    const auto where = "modes[" + juce::String (index) + "]";

    Mode m;
    m.name = str (v, "name");
    checkKeys (v, "mode", { "name", "amp", "tags", "groups", "effects", "sequences",
                            "modulators", "sequenceTriggers", "strumKeys", "ccBindings",
                            "menuKeySwitches", "ui" });
    if (m.name.isEmpty())
        res.errors.add (where + ": mode missing \"name\"");

    m.amp = parseAmp (get (v, "amp"));

    if (auto* a = get (v, "tags").getArray())
        for (auto& t : *a)
        {
            Tag tag;
            checkKeys (t, "mode.tag", { "name", "polyphony" });
            tag.name      = str (t, "name");
            tag.polyphony = optI (t, "polyphony");
            m.tags.add (tag);
        }

    if (auto* a = get (v, "groups").getArray())
    {
        int i = 0;
        for (auto& g : *a)
            m.groups.add (parseGroup (g, res, where + " group[" + juce::String (i++) + "]"));
    }
    else
    {
        res.errors.add (where + ": mode missing \"groups\" array");
    }

    if (auto* a = get (v, "effects").getArray())    for (auto& e : *a) m.effects.add (parseEffect (e));
    if (auto* a = get (v, "sequences").getArray())  for (auto& e : *a) m.sequences.add (parseSequence (e));
    if (auto* a = get (v, "modulators").getArray()) for (auto& e : *a) m.modulators.add (parseLfo (e));

    if (auto* a = get (v, "sequenceTriggers").getArray())
        for (auto& t : *a)
        {
            SequenceTrigger st;
            checkKeys (t, "sequenceTrigger", { "note", "sequence", "transpose", "rate",
                                               "loop", "trackVelocity", "swallow" });
            st.note          = intg (t, "note", 60);
            st.sequence      = intg (t, "sequence", 0);
            st.transpose     = intg (t, "transpose", 0);
            st.rate          = dbl (t, "rate", 10.0);
            st.loop          = boolean (t, "loop", false);
            st.trackVelocity = boolean (t, "trackVelocity", true);
            st.swallow       = boolean (t, "swallow", true);
            m.sequenceTriggers.add (st);
        }

    if (auto* a = get (v, "strumKeys").getArray())
        for (auto& e : *a)
        {
            checkKeys (e, "strumKey", { "note", "seqOffset", "rate" });
            StrumKey sk;
            sk.note      = intg (e, "note", -1);
            sk.seqOffset = intg (e, "seqOffset", 0);
            sk.rate      = optD (e, "rate");
            m.strumKeys.add (sk);
        }

    if (auto* a = get (v, "ccBindings").getArray())
        for (auto& e : *a)
        {
            CcBinding cb;
            checkKeys (e, "ccBinding", { "cc", "parameter", "targetId", "groupIndex",
                                         "controlIndex", "normMin", "normMax" });
            cb.cc           = intg (e, "cc", 1);
            cb.parameter    = str (e, "parameter");
            cb.targetId     = str (e, "targetId");
            cb.groupIndex   = optI (e, "groupIndex");
            cb.controlIndex = optI (e, "controlIndex");
            cb.normMin      = dbl (e, "normMin", 0.0);
            cb.normMax      = dbl (e, "normMax", 1.0);
            m.ccBindings.add (cb);
        }

    if (auto* a = get (v, "menuKeySwitches").getArray())
        for (auto& e : *a)
        {
            MenuKeySwitch ks;
            checkKeys (e, "menuKeySwitch", { "note", "option" });
            ks.note   = intg (e, "note", 0);
            ks.option = intg (e, "option", 0);
            m.menuKeySwitches.add (ks);
        }

    m.ui = parseUi (get (v, "ui"));
    return m;
}


// ---------------------------------------------------------------------------
// Referential validation (warnings). A typo'd targetId / out-of-range index is
// otherwise a SILENT no-op at runtime — the binding simply never fires.
// ---------------------------------------------------------------------------
void validateMode (const Mode& m, const juce::String& where, juce::StringArray& warnings)
{
    // Every id a binding may legally target.
    juce::StringArray ids, buttonIds;
    for (const auto& e : m.effects)      if (e.id.isNotEmpty())   ids.add (e.id);
    for (const auto& g : m.groups)       if (g.uid.isNotEmpty())  ids.add (g.uid);
    for (const auto& md : m.modulators)  if (md.id.isNotEmpty())  ids.add (md.id);
    for (const auto& tab : m.ui.tabs)
    {
        for (const auto& c : tab.controls) if (c.id.isNotEmpty()) ids.add (c.id);
        for (const auto& b : tab.buttons)  if (b.id.isNotEmpty()) { ids.add (b.id); buttonIds.add (b.id); }
        for (const auto& im : tab.images)  if (im.id.isNotEmpty()) ids.add (im.id);
        for (const auto& mn : tab.menus)   if (mn.id.isNotEmpty()) ids.add (mn.id);
    }

    auto checkBinding = [&] (const Binding& b, const juce::String& ctx)
    {
        if (b.targetId.isNotEmpty() && ! ids.contains (b.targetId))
            warnings.addIfNotAlreadyThere (where + " " + ctx + ": binding targetId \"" + b.targetId
                                           + "\" matches no effect/group/modulator/ui id");
    };

    for (const auto& tab : m.ui.tabs)
    {
        for (const auto& c : tab.controls)
            for (const auto& b : c.bindings)
                checkBinding (b, "control \"" + c.label + "\"");
        for (const auto& btn : tab.buttons)
            for (const auto& st : btn.states)
                for (const auto& b : st.bindings)
                    checkBinding (b, "button \"" + btn.id + "\"");
        for (const auto& mn : tab.menus)
        {
            for (const auto& o : mn.options)
                for (const auto& b : o.bindings)
                    checkBinding (b, "menu option \"" + o.name + "\"");
            if (! mn.options.isEmpty() && (mn.value < 1 || mn.value > mn.options.size()))
                warnings.add (where + ": menu \"" + mn.id + "\" default value "
                              + juce::String (mn.value) + " is outside 1.."
                              + juce::String (mn.options.size()));
        }
    }
    for (const auto& md : m.modulators)
        for (const auto& b : md.bindings)
            checkBinding (b, "modulator \"" + md.id + "\"");

    for (const auto& st : m.sequenceTriggers)
        if (st.sequence < 0 || st.sequence >= m.sequences.size())
            warnings.add (where + ": sequenceTrigger (note " + juce::String (st.note)
                          + ") references sequence " + juce::String (st.sequence)
                          + " but the mode has " + juce::String (m.sequences.size()));

    for (const auto& cb : m.ccBindings)
        if (cb.targetId.isNotEmpty() && ! ids.contains (cb.targetId))
            warnings.add (where + ": ccBinding (cc " + juce::String (cb.cc)
                          + ") targetId \"" + cb.targetId + "\" matches no ui id");

    const int nButtons = m.ui.tabs.isEmpty() ? 0 : m.ui.tabs.getReference (0).buttons.size();
    for (const auto& bl : m.ui.buttonLinks)
    {
        if (bl.fromId.isNotEmpty() && ! buttonIds.contains (bl.fromId))
            warnings.add (where + ": buttonLink fromId \"" + bl.fromId + "\" matches no button id");
        if (bl.toId.isNotEmpty() && ! buttonIds.contains (bl.toId))
            warnings.add (where + ": buttonLink toId \"" + bl.toId + "\" matches no button id");
        if (bl.fromId.isEmpty() && (bl.fromButton < 0 || bl.fromButton >= nButtons))
            warnings.add (where + ": buttonLink fromButton " + juce::String (bl.fromButton)
                          + " is outside the tab's " + juce::String (nButtons) + " buttons");
        if (bl.toId.isEmpty() && (bl.toButton < 0 || bl.toButton >= nButtons))
            warnings.add (where + ": buttonLink toButton " + juce::String (bl.toButton)
                          + " is outside the tab's " + juce::String (nButtons) + " buttons");
    }

    auto knownEffect = [] (const juce::String& t)
    {
        return t == "lowpass" || t == "highpass" || t == "convolution" || t == "gain"
            || t == "wave_shaper" || t == "chorus" || t == "phaser";
    };
    for (const auto& e : m.effects)
        if (! knownEffect (e.type))
            warnings.addIfNotAlreadyThere (where + ": effect type \"" + e.type
                                           + "\" is not supported by the engine (passthrough)");
    for (const auto& g : m.groups)
        for (const auto& e : g.effects)
            if (! knownEffect (e.type))
                warnings.addIfNotAlreadyThere (where + ": group effect type \"" + e.type
                                               + "\" is not supported by the engine (passthrough)");
}

} // namespace

ManifestParseResult loadManifest (const var& root)
{
    ManifestParseResult res;

    if (! root.isObject())
    {
        res.errors.add ("manifest root is not a JSON object");
        return res;
    }

    ScopedLintSink lint (res.warnings);   // unknown-key warnings land in res.warnings
    checkKeys (root, "manifest", { "schema", "format", "library", "gainDb",
                                   "polySaveDefault", "retriggerMuteDefault", "airSupply", "modes" });

    res.library.schema  = intg (root, "schema", 0);
    res.library.format  = str (root, "format");
    res.library.library = str (root, "library");
    res.library.gainDb  = optD (root, "gainDb").value_or (0.0);
    res.library.polySaveDefault = optB (root, "polySaveDefault").value_or (true);
    res.library.retriggerMuteDefault = optB (root, "retriggerMuteDefault").value_or (true);
    if (const auto air = get (root, "airSupply"); air.isObject())
    {
        checkKeys (air, "airSupply", { "volume", "brightness", "attack", "tags" });
        AirSupply a;
        a.volume     = optD (air, "volume").value_or (a.volume);
        a.brightness = optD (air, "brightness").value_or (a.brightness);
        a.attack     = optD (air, "attack").value_or (a.attack);
        a.tags       = strArray (air, "tags");
        res.library.airSupply = a;
    }

    if (res.library.schema == 0)
        res.warnings.add ("manifest has no \"schema\" version; assuming "
                          + juce::String (kManifestSchemaVersion));
    else if (res.library.schema > kManifestSchemaVersion)
        res.errors.add ("manifest schema " + juce::String (res.library.schema)
                        + " is newer than this engine supports ("
                        + juce::String (kManifestSchemaVersion) + ")");

    auto* modes = get (root, "modes").getArray();
    if (modes == nullptr)
    {
        res.errors.add ("manifest missing \"modes\" array");
        return res;
    }
    if (modes->isEmpty())
        res.warnings.add ("manifest \"modes\" array is empty");

    int i = 0;
    for (auto& m : *modes)
        res.library.modes.add (parseMode (m, res, i++));

    for (int mi = 0; mi < res.library.modes.size(); ++mi)
        validateMode (res.library.modes.getReference (mi),
                      "modes[" + juce::String (mi) + "]", res.warnings);

    res.ok = res.errors.isEmpty();
    return res;
}

ManifestParseResult loadManifestFromJson (const juce::String& jsonText)
{
    ManifestParseResult res;

    var root;
    auto r = juce::JSON::parse (jsonText, root);
    if (r.failed())
    {
        res.errors.add ("JSON parse error: " + r.getErrorMessage());
        return res;
    }
    return loadManifest (root);
}

// --- split-manifest resolution ($use / $ref) --------------------------------
namespace
{
// Deep copy so merges never mutate a cached partial (var objects are shared refs).
var cloneVar (const var& v)
{
    if (auto* arr = v.getArray())
    {
        juce::Array<var> out;
        for (auto& e : *arr) out.add (cloneVar (e));
        return var (out);
    }
    if (auto* obj = v.getDynamicObject())
    {
        auto* clone = new juce::DynamicObject();
        for (auto& p : obj->getProperties())
            clone->setProperty (p.name, cloneVar (p.value));
        return var (clone);
    }
    return v;   // primitive — immutable, safe to share
}

// Objects merge key-by-key recursively; anything else (incl. arrays) → overlay wins.
var deepMerge (const var& base, const var& overlay)
{
    auto* bo = base.getDynamicObject();
    auto* oo = overlay.getDynamicObject();
    if (bo == nullptr || oo == nullptr)
        return cloneVar (overlay);

    auto* out = new juce::DynamicObject();
    for (auto& p : bo->getProperties())
        out->setProperty (p.name, cloneVar (p.value));
    for (auto& p : oo->getProperties())
    {
        if (out->hasProperty (p.name))
            out->setProperty (p.name, deepMerge (out->getProperty (p.name), p.value));
        else
            out->setProperty (p.name, cloneVar (p.value));
    }
    return var (out);
}

using PartialLoader = std::function<var (const juce::String&)>;

var resolveNode (const var&, const PartialLoader&, juce::StringArray&, juce::StringArray);

// Load a named partial, resolve its own $use/$ref, guarding against import cycles.
var expandPartial (const juce::String& name, const PartialLoader& loader,
                   juce::StringArray& errors, juce::StringArray visiting)
{
    if (visiting.contains (name))
    {
        errors.add ("partial \"" + name + "\" forms an import cycle ("
                    + visiting.joinIntoString (" -> ") + " -> " + name + ")");
        return var (new juce::DynamicObject());
    }
    var p = loader (name);
    if (p.isVoid())
    {
        errors.add ("unknown partial \"" + name + "\"");
        return var (new juce::DynamicObject());
    }
    visiting.add (name);   // by-value copy: chain-local, so sibling $refs don't collide
    return resolveNode (p, loader, errors, visiting);
}

// Recursively expand $use (whole-partial base) and $ref (splice one partial), with
// the node's own fields deep-merged last (they win).
var resolveNode (const var& node, const PartialLoader& loader,
                 juce::StringArray& errors, juce::StringArray visiting)
{
    if (auto* arr = node.getArray())
    {
        juce::Array<var> out;
        for (auto& e : *arr) out.add (resolveNode (e, loader, errors, visiting));
        return var (out);
    }

    auto* obj = node.getDynamicObject();
    if (obj == nullptr)
        return node;   // primitive

    // { "$ref": "name", ...overrides }  → that partial with the overrides merged on top.
    if (obj->hasProperty ("$ref"))
    {
        var base = expandPartial (obj->getProperty ("$ref").toString(), loader, errors, visiting);
        auto* overrides = new juce::DynamicObject();
        for (auto& p : obj->getProperties())
            if (p.name.toString() != "$ref")
                overrides->setProperty (p.name, p.value);
        return deepMerge (base, resolveNode (var (overrides), loader, errors, visiting));
    }

    // "$use": [...] partials form the base; this node's own keys deep-merge on top.
    var result (new juce::DynamicObject());
    if (obj->hasProperty ("$use"))
    {
        if (auto* uses = obj->getProperty ("$use").getArray())
            for (auto& u : *uses)
                result = deepMerge (result, expandPartial (u.toString(), loader, errors, visiting));
        else
            errors.add ("\"$use\" must be an array of partial names");
    }

    auto* own = new juce::DynamicObject();
    for (auto& p : obj->getProperties())
        if (p.name.toString() != "$use")
            own->setProperty (p.name, resolveNode (p.value, loader, errors, visiting));

    return deepMerge (result, var (own));
}
} // namespace

var resolveSplitManifest (const var& index,
                          const std::function<var (const juce::String&)>& modeLoader,
                          const std::function<var (const juce::String&)>& partialLoader,
                          juce::StringArray& errors)
{
    auto* indexObj = index.getDynamicObject();
    if (indexObj == nullptr)
    {
        errors.add ("manifest index is not a JSON object");
        return {};
    }

    // Root carries everything from the index except "modes" (resolved from files).
    auto* root = new juce::DynamicObject();
    for (auto& p : indexObj->getProperties())
        if (p.name.toString() != "modes")
            root->setProperty (p.name, cloneVar (p.value));

    juce::Array<var> modesOut;
    if (auto* modes = index.getProperty ("modes", var()).getArray())
    {
        for (auto& entry : *modes)
        {
            // A string names modes/<name>.json; an inline object is used as-is.
            var modeVar = entry.isString() ? modeLoader (entry.toString()) : entry;
            if (modeVar.isVoid())
            {
                errors.add ("mode \"" + entry.toString() + "\" could not be loaded");
                continue;
            }
            modesOut.add (resolveNode (modeVar, partialLoader, errors, {}));
        }
    }
    else
    {
        errors.add ("manifest index missing \"modes\" array");
    }

    root->setProperty ("modes", var (modesOut));
    return var (root);
}

ManifestParseResult loadManifestFromFolder (const juce::File& manifestDir)
{
    ManifestParseResult res;

    auto indexFile = manifestDir.getChildFile ("index.json");
    if (! indexFile.existsAsFile())
    {
        res.errors.add ("no index.json in " + manifestDir.getFullPathName());
        return res;
    }

    var index;
    if (auto r = juce::JSON::parse (indexFile.loadFileAsString(), index); r.failed())
    {
        res.errors.add ("index.json parse error: " + r.getErrorMessage());
        return res;
    }

    auto loadJsonFile = [] (const juce::File& f) -> var
    {
        if (! f.existsAsFile()) return {};
        var v;
        return juce::JSON::parse (f.loadFileAsString(), v).failed() ? var() : v;
    };
    auto modeLoader    = [&] (const juce::String& n) { return loadJsonFile (manifestDir.getChildFile ("modes").getChildFile (n + ".json")); };
    auto partialLoader = [&] (const juce::String& n) { return loadJsonFile (manifestDir.getChildFile ("partials").getChildFile (n + ".json")); };

    juce::StringArray resolveErrors;
    var merged = resolveSplitManifest (index, modeLoader, partialLoader, resolveErrors);
    if (! resolveErrors.isEmpty())
    {
        res.errors.addArray (resolveErrors);
        return res;
    }

    return loadManifest (merged);
}

} // namespace dm
