#include "ManifestWriter.h"

namespace dm
{

namespace
{
using juce::var;

// A fresh JSON object. Returns a var owning a DynamicObject.
struct Obj
{
    juce::DynamicObject::Ptr o { new juce::DynamicObject() };

    void set (const char* key, const var& value) { o->setProperty (key, value); }

    void setStr (const char* key, const juce::String& s) { if (s.isNotEmpty()) o->setProperty (key, s); }

    template <typename T>
    void setOpt (const char* key, const std::optional<T>& v) { if (v) o->setProperty (key, *v); }

    var toVar() const { return var (o.get()); }
};

var fromArray (const juce::Array<var>& items)
{
    return var (items);
}

var writeBinding (const Binding& b)
{
    Obj o;
    o.setStr ("type", b.type);
    o.setStr ("level", b.level);
    o.setStr ("targetId", b.targetId);
    o.setStr ("identifier", b.identifier);
    o.setStr ("translationTable", b.translationTable);
    if (b.translationReversed) o.set ("translationReversed", true);
    o.setStr ("parameter", b.parameter);
    o.setStr ("translation", b.translation);
    o.setStr ("modBehavior", b.modBehavior);

    o.setOpt ("factor", b.factor);
    o.setOpt ("modAmount", b.modAmount);
    o.setOpt ("translationOutputMin", b.translationOutputMin);
    o.setOpt ("translationOutputMax", b.translationOutputMax);

    o.setOpt ("effectIndex", b.effectIndex);
    o.setOpt ("controlIndex", b.controlIndex);
    o.setOpt ("groupIndex", b.groupIndex);
    o.setOpt ("noteIndex", b.noteIndex);
    o.setOpt ("bindingIndex", b.bindingIndex);
    o.setOpt ("seqIndex", b.seqIndex);
    o.setOpt ("position", b.position);

    if (! b.translationValue.isVoid())
        o.set ("translationValue", b.translationValue);

    return o.toVar();
}

var writeBindings (const juce::Array<Binding>& bindings)
{
    juce::Array<var> arr;
    for (const auto& b : bindings)
        arr.add (writeBinding (b));
    return fromArray (arr);
}

var writeAmp (const AmpEnvelope& a)
{
    Obj o;
    o.set ("attack", a.attack);
    o.set ("decay", a.decay);
    o.set ("sustain", a.sustain);
    o.set ("release", a.release);
    o.set ("volume", a.volume);
    o.set ("velTrack", a.velTrack);
    o.set ("enabled", a.enabled);
    o.setOpt ("attackCurve", a.attackCurve);
    o.setOpt ("decayCurve", a.decayCurve);
    o.setOpt ("releaseCurve", a.releaseCurve);
    return o.toVar();
}

var writeSample (const Sample& s)
{
    Obj o;
    o.set ("source", s.source);
    o.set ("loNote", s.loNote);
    o.set ("hiNote", s.hiNote);
    o.set ("rootNote", s.rootNote);
    o.setOpt ("lengthFrames", s.lengthFrames);
    o.setOpt ("sampleRate", s.sampleRate);
    o.set ("pitchKeyTrack", s.pitchKeyTrack);
    o.setOpt ("pitchDrift", s.pitchDrift);
    o.setOpt ("start", s.start);
    o.setOpt ("end", s.end);
    o.setOpt ("volume", s.volume);
    o.setOpt ("seqPosition", s.seqPosition);
    o.setOpt ("ampEnvEnabled", s.ampEnvEnabled);
    o.setOpt ("onLoCC64", s.onLoCC64);
    o.setOpt ("onHiCC64", s.onHiCC64);

    if (s.loop.enabled)
    {
        Obj loop;
        loop.set ("enabled", true);
        loop.setOpt ("start", s.loop.start);
        loop.setOpt ("end", s.loop.end);
        loop.setOpt ("crossfade", s.loop.crossfade);
        o.set ("loop", loop.toVar());
    }
    return o.toVar();
}

var writeStringArray (const juce::StringArray& a)
{
    juce::Array<var> arr;
    for (const auto& s : a)
        arr.add (s);
    return fromArray (arr);
}

var writeEffect (const Effect& e);   // defined below

var writeGroup (const Group& g)
{
    Obj o;
    o.setStr ("uid", g.uid);
    if (! g.tags.isEmpty())
        o.set ("tags", writeStringArray (g.tags));
    o.setStr ("trigger", g.trigger);
    o.setStr ("loopCrossfadeMode", g.loopCrossfadeMode);

    if (g.velocity)
    {
        Obj v;
        v.set ("lo", g.velocity->lo);
        v.set ("hi", g.velocity->hi);
        o.set ("velocity", v.toVar());
    }
    if (g.roundRobin)
    {
        Obj rr;
        rr.setStr ("mode", g.roundRobin->mode);
        rr.setOpt ("length", g.roundRobin->length);
        o.set ("roundRobin", rr.toVar());
    }
    if (g.silencing)
    {
        Obj s;
        s.setStr ("mode", g.silencing->mode);
        if (! g.silencing->byTags.isEmpty())
            s.set ("byTags", writeStringArray (g.silencing->byTags));
        o.set ("silencing", s.toVar());
    }

    o.setOpt ("attack", g.attack);
    o.setOpt ("decay", g.decay);
    o.setOpt ("sustain", g.sustain);
    o.setOpt ("release", g.release);
    o.setOpt ("volume", g.volume);
    o.setOpt ("velTrack", g.velTrack);
    o.setOpt ("ampEnvEnabled", g.ampEnvEnabled);
    o.setOpt ("pitchKeyTrack", g.pitchKeyTrack);

    if (! g.effects.isEmpty())
    {
        juce::Array<var> effects;
        for (const auto& e : g.effects)
            effects.add (writeEffect (e));
        o.set ("effects", fromArray (effects));
    }

    juce::Array<var> samples;
    for (const auto& s : g.samples)
        samples.add (writeSample (s));
    o.set ("samples", fromArray (samples));

    return o.toVar();
}

var writeEffect (const Effect& e)
{
    Obj o;
    o.setStr ("id", e.id);
    o.setStr ("type", e.type);
    o.set ("enabled", e.enabled);
    o.setOpt ("frequency", e.frequency);
    o.setOpt ("resonance", e.resonance);
    o.setOpt ("gain", e.gain);
    o.setOpt ("drive", e.drive);
    o.setOpt ("mix", e.mix);
    o.setOpt ("wet", e.wet);
    o.setOpt ("outputLevel", e.outputLevel);
    o.setOpt ("rate", e.rate);
    o.setOpt ("depth", e.depth);
    o.setOpt ("feedback", e.feedback);
    o.setStr ("ir", e.ir);
    if (! e.normalizeIr) o.set ("normalizeIr", false);
    return o.toVar();
}

var writeLfo (const Lfo& l)
{
    Obj o;
    o.setStr ("id", l.id);
    o.setStr ("shape", l.shape);
    o.set ("frequency", l.frequency);
    o.set ("modAmount", l.modAmount);
    o.set ("bindings", writeBindings (l.bindings));
    return o.toVar();
}

var writeSequence (const NoteSequence& seq)
{
    Obj o;
    o.setStr ("name", seq.name);
    o.setOpt ("length", seq.length);
    o.setOpt ("rate", seq.rate);

    juce::Array<var> notes;
    for (const auto& n : seq.notes)
    {
        Obj no;
        no.set ("position", n.position);
        no.set ("note", n.note);
        no.set ("velocity", n.velocity);
        no.set ("length", n.length);
        no.set ("enabled", n.enabled);
        no.set ("swallowNotes", n.swallowNotes);
        notes.add (no.toVar());
    }
    o.set ("notes", fromArray (notes));
    return o.toVar();
}

var writeRect (const Rect& r)
{
    Obj o;
    o.set ("x", r.x);
    o.set ("y", r.y);
    o.set ("width", r.width);
    o.set ("height", r.height);
    return o.toVar();
}

var writeControl (const Control& c)
{
    Obj o;
    o.setStr ("id", c.id);
    o.set ("rect", writeRect (c.rect));
    o.setStr ("label", c.label);
    o.setStr ("valueType", c.valueType);
    o.setOpt ("min", c.min);
    o.setOpt ("max", c.max);
    o.setOpt ("value", c.value);
    o.setStr ("textColor", c.textColor);
    o.setStr ("style", c.style);
    if (c.skin)
    {
        Obj s;
        s.setStr ("image", c.skin->image);
        s.setOpt ("numFrames", c.skin->numFrames);
        s.setStr ("orientation", c.skin->orientation);
        o.set ("skin", s.toVar());
    }
    o.setOpt ("mouseDragSensitivity", c.mouseDragSensitivity);
    o.setOpt ("controlIndex", c.controlIndex);
    if (! c.visible) o.set ("visible", false);
    o.set ("bindings", writeBindings (c.bindings));
    return o.toVar();
}

var writeButton (const Button& b)
{
    Obj o;
    o.setStr ("id", b.id);
    o.set ("rect", writeRect (b.rect));
    o.setStr ("style", b.style);
    o.setOpt ("value", b.value);
    o.setOpt ("controlIndex", b.controlIndex);
    if (! b.visible) o.set ("visible", false);

    juce::Array<var> states;
    for (const auto& st : b.states)
    {
        Obj so;
        so.setStr ("name", st.name);
        so.setStr ("mainImage", st.mainImage);
        so.setStr ("hoverImage", st.hoverImage);
        so.setStr ("clickImage", st.clickImage);
        so.set ("bindings", writeBindings (st.bindings));
        states.add (so.toVar());
    }
    o.set ("states", fromArray (states));
    return o.toVar();
}

var writeImage (const UiImage& img)
{
    Obj o;
    o.setStr ("id", img.id);
    o.set ("rect", writeRect (img.rect));
    o.setStr ("image", img.image);
    o.setStr ("aspectRatioMode", img.aspectRatioMode);
    o.setOpt ("controlIndex", img.controlIndex);
    if (! img.visible) o.set ("visible", false);
    return o.toVar();
}

var writeUi (const Ui& ui)
{
    Obj o;
    o.setStr ("background", ui.background);
    o.set ("width", ui.width);
    o.set ("height", ui.height);
    if (ui.cropTop != 0) o.set ("cropTop", ui.cropTop);
    o.setStr ("layoutMode", ui.layoutMode);
    o.setStr ("bgMode", ui.bgMode);
    if (ui.whiteKeyTint.isNotEmpty()) o.setStr ("whiteKeyTint", ui.whiteKeyTint);
    if (ui.blackKeyTint.isNotEmpty()) o.setStr ("blackKeyTint", ui.blackKeyTint);

    juce::Array<var> tabs;
    for (const auto& t : ui.tabs)
    {
        Obj to;
        to.setStr ("name", t.name);

        juce::Array<var> controls;
        for (const auto& c : t.controls) controls.add (writeControl (c));
        to.set ("controls", fromArray (controls));

        juce::Array<var> buttons;
        for (const auto& b : t.buttons) buttons.add (writeButton (b));
        to.set ("buttons", fromArray (buttons));

        juce::Array<var> images;
        for (const auto& im : t.images) images.add (writeImage (im));
        to.set ("images", fromArray (images));

        if (! t.menus.isEmpty())
        {
            juce::Array<var> menus;
            for (const auto& m : t.menus)
            {
                Obj mo;
                mo.setStr ("id", m.id);
                mo.set ("rect", writeRect (m.rect));
                mo.set ("value", m.value);
                mo.setStr ("textColor", m.textColor);
                mo.setStr ("backgroundColor", m.backgroundColor);
                mo.setStr ("hAlign", m.hAlign);
                mo.setOpt ("controlIndex", m.controlIndex);
                if (! m.visible) mo.set ("visible", false);
                juce::Array<var> opts;
                for (const auto& op : m.options)
                {
                    Obj oo;
                    oo.set ("name", op.name);
                    oo.set ("seqIndex", op.seqIndex);
                    if (! op.bindings.isEmpty())
                        oo.set ("bindings", writeBindings (op.bindings));
                    opts.add (oo.toVar());
                }
                mo.set ("options", fromArray (opts));
                menus.add (mo.toVar());
            }
            to.set ("menus", fromArray (menus));
        }

        tabs.add (to.toVar());
    }
    o.set ("tabs", fromArray (tabs));

    Obj kb;
    juce::Array<var> colors;
    for (const auto& kc : ui.keyboardColors)
    {
        Obj co;
        co.set ("loNote", kc.loNote);
        co.set ("hiNote", kc.hiNote);
        co.setStr ("color", kc.color);
        colors.add (co.toVar());
    }
    kb.set ("colors", fromArray (colors));
    o.set ("keyboard", kb.toVar());

    if (! ui.buttonLinks.isEmpty())
    {
        juce::Array<var> links;
        for (const auto& bl : ui.buttonLinks)
        {
            Obj lo;
            lo.set ("fromButton", bl.fromButton);
            lo.set ("fromState", bl.fromState);
            lo.set ("toButton", bl.toButton);
            lo.set ("toState", bl.toState);
            if (bl.fromId.isNotEmpty()) lo.set ("fromId", bl.fromId);
            if (bl.toId.isNotEmpty())   lo.set ("toId",   bl.toId);
            links.add (lo.toVar());
        }
        o.set ("buttonLinks", fromArray (links));
    }

    return o.toVar();
}

var writeMode (const Mode& m)
{
    Obj o;
    o.set ("name", m.name);
    o.set ("amp", writeAmp (m.amp));

    if (! m.tags.isEmpty())
    {
        juce::Array<var> tags;
        for (const auto& t : m.tags)
        {
            Obj to;
            to.setStr ("name", t.name);
            to.setOpt ("polyphony", t.polyphony);
            tags.add (to.toVar());
        }
        o.set ("tags", fromArray (tags));
    }

    juce::Array<var> groups;
    for (const auto& g : m.groups) groups.add (writeGroup (g));
    o.set ("groups", fromArray (groups));

    if (! m.effects.isEmpty())
    {
        juce::Array<var> effects;
        for (const auto& e : m.effects) effects.add (writeEffect (e));
        o.set ("effects", fromArray (effects));
    }
    if (! m.sequences.isEmpty())
    {
        juce::Array<var> seqs;
        for (const auto& s : m.sequences) seqs.add (writeSequence (s));
        o.set ("sequences", fromArray (seqs));
    }
    if (! m.sequenceTriggers.isEmpty())
    {
        juce::Array<var> triggers;
        for (const auto& t : m.sequenceTriggers)
        {
            Obj to;
            to.set ("note", t.note);
            to.set ("sequence", t.sequence);
            to.set ("transpose", t.transpose);
            to.set ("rate", t.rate);
            to.set ("loop", t.loop);
            to.set ("trackVelocity", t.trackVelocity);
            to.set ("swallow", t.swallow);
            triggers.add (to.toVar());
        }
        o.set ("sequenceTriggers", fromArray (triggers));
    }
    if (! m.modulators.isEmpty())
    {
        juce::Array<var> mods;
        for (const auto& l : m.modulators) mods.add (writeLfo (l));
        o.set ("modulators", fromArray (mods));
    }

    if (! m.ccBindings.isEmpty())
    {
        juce::Array<var> ccs;
        for (const auto& cb : m.ccBindings)
        {
            Obj co;
            co.set ("cc", cb.cc);
            co.setStr ("parameter", cb.parameter);
            co.setStr ("targetId", cb.targetId);
            co.setOpt ("groupIndex", cb.groupIndex);
            co.setOpt ("controlIndex", cb.controlIndex);
            co.set ("normMin", cb.normMin);
            co.set ("normMax", cb.normMax);
            ccs.add (co.toVar());
        }
        o.set ("ccBindings", fromArray (ccs));
    }

    if (! m.menuKeySwitches.isEmpty())
    {
        juce::Array<var> keys;
        for (const auto& ks : m.menuKeySwitches)
        {
            Obj ko;
            ko.set ("note", ks.note);
            ko.set ("option", ks.option);
            keys.add (ko.toVar());
        }
        o.set ("menuKeySwitches", fromArray (keys));
    }

    o.set ("ui", writeUi (m.ui));
    return o.toVar();
}
} // namespace

var manifestToVar (const PresetLibrary& library)
{
    Obj o;
    o.set ("schema", library.schema > 0 ? library.schema : kManifestSchemaVersion);
    o.setStr ("format", library.format.isNotEmpty() ? library.format : juce::String ("dmse-manifest"));
    o.setStr ("library", library.library);
    if (library.gainDb != 0.0)
        o.set ("gainDb", library.gainDb);
    if (! library.polySaveDefault)
        o.set ("polySaveDefault", false);

    juce::Array<var> modes;
    for (const auto& m : library.modes)
        modes.add (writeMode (m));
    o.set ("modes", fromArray (modes));

    return o.toVar();
}

juce::String writeManifestToJson (const PresetLibrary& library, bool oneLine)
{
    return juce::JSON::toString (manifestToVar (library), oneLine);
}

namespace
{
// A mode name → a lowercase, hyphen-separated filename stem ("Chords (Looped)" →
// "chords-looped"). Empty names fall back to "mode-<n>".
juce::String slugify (const juce::String& name, int index)
{
    const auto lower = name.toLowerCase();
    juce::String s;
    for (int i = 0; i < lower.length(); ++i)
    {
        const juce::juce_wchar c = lower[i];
        if (juce::CharacterFunctions::isLetterOrDigit (c))
            s << juce::String::charToString (c);
        else if (! s.endsWithChar ('-'))
            s << '-';
    }
    s = s.trimCharactersAtStart ("-").trimCharactersAtEnd ("-");
    return s.isEmpty() ? ("mode-" + juce::String (index + 1)) : s;
}
} // namespace

bool writeSplitManifest (const PresetLibrary& library, const juce::File& manifestDir)
{
    const var root = manifestToVar (library);
    auto* rootObj = root.getDynamicObject();
    if (rootObj == nullptr)
        return false;

    auto modesDir = manifestDir.getChildFile ("modes");
    if (manifestDir.createDirectory().failed() || modesDir.createDirectory().failed())
        return false;
    // Drop any stale per-mode files from a previous run (mode renamed/removed).
    for (auto& f : modesDir.findChildFiles (juce::File::findFiles, false, "*.json"))
        f.deleteFile();

    juce::Array<var> modeVars;
    if (auto* modes = root.getProperty ("modes", var()).getArray())
        modeVars = *modes;

    juce::StringArray stems;
    for (int i = 0; i < modeVars.size(); ++i)
    {
        auto stem = slugify (modeVars[i].getProperty ("name", var()).toString(), i);
        auto unique = stem;
        for (int n = 2; stems.contains (unique); ++n)   // de-dup colliding slugs
            unique = stem + "-" + juce::String (n);
        stems.add (unique);

        if (! modesDir.getChildFile (unique + ".json")
                 .replaceWithText (juce::JSON::toString (modeVars[i], false)))
            return false;
    }

    // index.json = everything from root except "modes", plus the ordered stem list.
    auto* index = new juce::DynamicObject();
    for (auto& p : rootObj->getProperties())
        if (p.name.toString() != "modes")
            index->setProperty (p.name, p.value);
    juce::Array<var> stemVars;
    for (auto& s : stems)
        stemVars.add (s);
    index->setProperty ("modes", var (stemVars));

    return manifestDir.getChildFile ("index.json")
               .replaceWithText (juce::JSON::toString (var (index), false));
}

} // namespace dm
