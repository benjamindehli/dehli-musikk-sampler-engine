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
    b.raw         = v;
    b.type        = str (v, "type");
    b.level       = str (v, "level");
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
    r.x      = intg (v, "x", 0);
    r.y      = intg (v, "y", 0);
    r.width  = intg (v, "width", 0);
    r.height = intg (v, "height", 0);
    return r;
}

AmpEnvelope parseAmp (const var& v)
{
    AmpEnvelope a;
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
    s.source = str (v, "source");
    if (s.source.isEmpty())
        res.errors.add (where + ": sample missing \"source\"");

    s.loNote   = intg (v, "loNote", 0);
    s.hiNote   = intg (v, "hiNote", 127);
    s.rootNote = intg (v, "rootNote", 60);

    s.lengthFrames  = optI (v, "lengthFrames");
    s.sampleRate    = optD (v, "sampleRate");
    s.pitchKeyTrack = boolean (v, "pitchKeyTrack", false);

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
    g.uid               = str (v, "uid");
    g.tags              = strArray (v, "tags");
    g.trigger           = str (v, "trigger");
    g.loopCrossfadeMode = str (v, "loopCrossfadeMode");

    if (auto vel = get (v, "velocity"); ! vel.isVoid())
    {
        VelocityRange vr;
        vr.lo = intg (vel, "lo", 0);
        vr.hi = intg (vel, "hi", 127);
        g.velocity = vr;
    }

    if (auto rr = get (v, "roundRobin"); ! rr.isVoid())
    {
        RoundRobin r;
        r.mode   = str (rr, "mode");
        r.length = optI (rr, "length");
        g.roundRobin = r;
    }

    if (auto sil = get (v, "silencing"); ! sil.isVoid())
    {
        Silencing s;
        s.mode   = str (sil, "mode");
        s.byTags = strArray (sil, "byTags");
        g.silencing = s;
    }

    g.decay         = optD (v, "decay");
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
    e.raw     = v;
    e.type    = str (v, "type");
    e.enabled = boolean (v, "enabled", true);

    e.frequency   = optD (v, "frequency");
    e.resonance   = optD (v, "resonance");
    e.gain        = optD (v, "gain");
    e.drive       = optD (v, "drive");
    e.mix         = optD (v, "mix");
    e.wet         = optD (v, "wet");
    e.outputLevel = optD (v, "outputLevel");
    e.ir          = str (v, "ir");
    e.normalizeIr = boolean (v, "normalizeIr", true);
    return e;
}

Lfo parseLfo (const var& v)
{
    Lfo l;
    l.shape     = str (v, "shape");
    l.frequency = dbl (v, "frequency", 0.0);
    l.modAmount = dbl (v, "modAmount", 0.0);
    l.bindings  = parseBindings (v);
    return l;
}

NoteSequence parseSequence (const var& v)
{
    NoteSequence seq;
    seq.name   = str (v, "name");
    seq.length = optI (v, "length");
    seq.rate   = optD (v, "rate");

    if (auto* a = get (v, "notes").getArray())
    {
        for (auto& n : *a)
        {
            SequenceNote note;
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
    skin.image       = str (s, "image");
    skin.numFrames   = optI (s, "numFrames");
    skin.orientation = str (s, "orientation");
    return skin;
}

Control parseControl (const var& v)
{
    Control c;
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
    c.bindings  = parseBindings (v);
    return c;
}

Button parseButton (const var& v)
{
    Button b;
    b.rect  = parseRect (get (v, "rect"));
    b.style = str (v, "style");
    b.value = optI (v, "value");

    if (auto* a = get (v, "states").getArray())
    {
        for (auto& s : *a)
        {
            ButtonState st;
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
    img.rect            = parseRect (get (v, "rect"));
    img.image           = str (v, "image");
    img.aspectRatioMode = str (v, "aspectRatioMode");
    img.controlIndex    = optI (v, "controlIndex");
    return img;
}

Ui parseUi (const var& v)
{
    Ui ui;
    ui.background = str (v, "background");
    ui.width      = intg (v, "width", 0);
    ui.height     = intg (v, "height", 0);
    ui.layoutMode = str (v, "layoutMode");
    ui.bgMode     = str (v, "bgMode");

    if (auto* tabs = get (v, "tabs").getArray())
    {
        for (auto& t : *tabs)
        {
            Tab tab;
            tab.name = str (t, "name");
            if (auto* a = get (t, "controls").getArray()) for (auto& e : *a) tab.controls.add (parseControl (e));
            if (auto* a = get (t, "buttons").getArray())  for (auto& e : *a) tab.buttons.add  (parseButton (e));
            if (auto* a = get (t, "images").getArray())   for (auto& e : *a) tab.images.add   (parseImage (e));
            if (auto* a = get (t, "menus").getArray())
                for (auto& e : *a)
                {
                    Menu menu;
                    menu.rect  = parseRect (get (e, "rect"));
                    menu.value = intg (e, "value", 1);
                    menu.textColor       = str (e, "textColor");
                    menu.backgroundColor = str (e, "backgroundColor");
                    menu.hAlign          = str (e, "hAlign");
                    if (auto* opts = get (e, "options").getArray())
                        for (auto& o : *opts)
                        {
                            MenuOption mo;
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
            kc.loNote = intg (c, "loNote", 0);
            kc.hiNote = intg (c, "hiNote", 127);
            kc.color  = str (c, "color");
            ui.keyboardColors.add (kc);
        }
    }
    return ui;
}

Mode parseMode (const var& v, ManifestParseResult& res, int index)
{
    const auto where = "modes[" + juce::String (index) + "]";

    Mode m;
    m.name = str (v, "name");
    if (m.name.isEmpty())
        res.errors.add (where + ": mode missing \"name\"");

    m.amp = parseAmp (get (v, "amp"));

    if (auto* a = get (v, "tags").getArray())
        for (auto& t : *a)
        {
            Tag tag;
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
            st.note          = intg (t, "note", 60);
            st.sequence      = intg (t, "sequence", 0);
            st.transpose     = intg (t, "transpose", 0);
            st.rate          = dbl (t, "rate", 10.0);
            st.loop          = boolean (t, "loop", false);
            st.trackVelocity = boolean (t, "trackVelocity", true);
            st.swallow       = boolean (t, "swallow", true);
            m.sequenceTriggers.add (st);
        }

    if (auto* a = get (v, "ccBindings").getArray())
        for (auto& e : *a)
        {
            CcBinding cb;
            cb.cc         = intg (e, "cc", 1);
            cb.parameter  = str (e, "parameter");
            cb.groupIndex = optI (e, "groupIndex");
            cb.normMin    = dbl (e, "normMin", 0.0);
            cb.normMax    = dbl (e, "normMax", 1.0);
            m.ccBindings.add (cb);
        }

    if (auto* a = get (v, "menuKeySwitches").getArray())
        for (auto& e : *a)
        {
            MenuKeySwitch ks;
            ks.note   = intg (e, "note", 0);
            ks.option = intg (e, "option", 0);
            m.menuKeySwitches.add (ks);
        }

    m.ui = parseUi (get (v, "ui"));
    return m;
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

    res.library.schema  = intg (root, "schema", 0);
    res.library.format  = str (root, "format");
    res.library.library = str (root, "library");
    res.library.gainDb  = optD (root, "gainDb").value_or (0.0);

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
