#include "ManifestUiComponent.h"
#include <model/TableEval.h>   // shared evalTableLinear (VISIBLE step tables / OPACITY ramps)
#include <cmath>
#include <vector>
#include <algorithm>

namespace dm
{

// ---------------------------------------------------------------------------
// Filmstrip rotary knob: a vertical strip of `frames` images; the frame shown is
// chosen by the normalised value. Vertical drag changes the value.
// ---------------------------------------------------------------------------
class ManifestUiComponent::FilmstripKnob : public juce::Component
{
public:
    FilmstripKnob (juce::Image strip, int numFrames, double minV, double maxV, double initial,
                   bool horizontalDrag = false, bool invertDrag = false)
        : film (strip), frames (juce::jmax (1, numFrames)), minVal (minV), maxVal (maxV),
          horizontal (horizontalDrag), inverted (invertDrag)
    {
        defaultVal = juce::jlimit (minVal, maxVal, initial);   // manifest value = reset target
        value = defaultVal;
    }

    std::function<void (double)> onChange;
    std::function<void (juce::Component&, const juce::String&)> onShowValue;   // live value readout
    std::function<void()> onHideValue;
    std::function<void()> onRightClick;   // context menu (MIDI Learn / Settings)

    double getValue() const noexcept { return value; }

    // Value formatted for the readout bubble: a fixed 2 decimals so the label width
    // stays constant while turning (no jitter from trimming trailing zeros).
    juce::String valueText() const
    {
        return juce::String (value, 2);
    }

    /** Set the displayed value WITHOUT firing onChange (used for external sync). */
    void setValue (double v)
    {
        const double nv = juce::jlimit (minVal, maxVal, v);
        if (! juce::exactlyEqual (nv, value)) { value = nv; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        if (! film.isValid())
            return;

        const int fw = film.getWidth();
        const int fh = juce::jmax (1, film.getHeight() / frames);
        const double norm = maxVal > minVal ? (value - minVal) / (maxVal - minVal) : 0.0;
        const int idx = juce::jlimit (0, frames - 1, (int) std::round (norm * (frames - 1)));

        g.drawImage (film, 0, 0, getWidth(), getHeight(),
                     0, idx * fh, fw, fh);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }

        // Option (macOS) / Ctrl (Windows) + click resets to the manifest default value.
       #if JUCE_MAC
        const bool resetClick = e.mods.isAltDown();       // Option key on macOS
       #else
        const bool resetClick = e.mods.isCommandDown();   // Ctrl on Windows/Linux
       #endif
        if (resetClick && ! juce::exactlyEqual (value, defaultVal))
        {
            value = defaultVal;
            repaint();
            if (onChange) onChange (value);
        }
        dragStartX = e.position.x;
        dragStartY = e.position.y;
        dragStartVal = value;
        if (onShowValue) onShowValue (*this, valueText());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double range = maxVal - minVal;
        if (range <= 0.0)
            return;

        // Horizontal faders drag left/right (right = more); everything else up/down (up = more).
        // Horizontal faders are short, so use a tighter px-per-sweep (more sensitive).
        // `inverted` flips the direction (DecentSampler's negative mouseDragSensitivity —
        // e.g. VCCO drawbars, where pulling DOWN increases like a real organ drawbar).
        double delta = horizontal ? (double) (e.position.x - dragStartX)
                                  : (double) (dragStartY - e.position.y);
        if (inverted)
            delta = -delta;
        const double pxPerSweep = horizontal ? 120.0 : 200.0;
        double norm = (dragStartVal - minVal) / range + delta / pxPerSweep;
        value = minVal + juce::jlimit (0.0, 1.0, norm) * range;
        repaint();
        if (onChange)
            onChange (value);
        if (onShowValue) onShowValue (*this, valueText());
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (onHideValue) onHideValue();
    }

private:
    juce::Image film;
    int frames;
    double minVal, maxVal, value { 0.0 };
    double defaultVal { 0.0 };
    bool horizontal { false };
    bool inverted { false };
    float dragStartX { 0.0f }, dragStartY { 0.0f };
    double dragStartVal { 0.0 };
};

// ---------------------------------------------------------------------------
// Image button: cycles through state images on click.
// ---------------------------------------------------------------------------
class ManifestUiComponent::ImageStateButton : public juce::Component
{
public:
    ImageStateButton (std::vector<juce::Image> imgs, int initial)
        : images (std::move (imgs)), state (initial) {}

    std::function<void (int)> onChange;
    std::function<void()> onRightClick;   // context menu (MIDI Learn / Settings)
    int getState() const noexcept { return state; }

    /** Set the state WITHOUT firing onChange (used for external sync). */
    void setState (int s)
    {
        if (s >= 0 && s < (int) images.size() && s != state) { state = s; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        if (state >= 0 && state < (int) images.size() && images[(size_t) state].isValid())
            g.drawImage (images[(size_t) state], getLocalBounds().toFloat(),
                         juce::RectanglePlacement::stretchToFit);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { if (onRightClick) onRightClick(); return; }
        if (images.size() < 2)
            return;
        state = (state + 1) % (int) images.size();
        repaint();
        if (onChange)
            onChange (state);
    }

private:
    std::vector<juce::Image> images;
    int state { 0 };
};

// ---------------------------------------------------------------------------
// Indicator image whose picture can be swapped (e.g. lights via PATH bindings).
// ---------------------------------------------------------------------------
class ManifestUiComponent::SwappableImage : public juce::Component
{
public:
    void setImage (juce::Image img) { image = img; repaint(); }

    /** OPACITY binding: 0..1 alpha (e.g. crossfading LED up/down indicators). */
    void setImageAlpha (float a)
    {
        const float na = juce::jlimit (0.0f, 1.0f, a);
        if (! juce::exactlyEqual (na, alpha)) { alpha = na; repaint(); }
    }

    void paint (juce::Graphics& g) override
    {
        if (image.isValid())
        {
            g.setOpacity (alpha);
            g.drawImage (image, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
        }
    }

private:
    juce::Image image;
    float alpha { 1.0f };
};

// ---------------------------------------------------------------------------

ManifestUiComponent::ManifestUiComponent (const Ui& ui, ImageProvider imageProvider)
    : uiData (ui), provider (std::move (imageProvider))
{
    if (uiData.background.isNotEmpty() && provider)
        background = provider (uiData.background);
    if (uiData.overlay.isNotEmpty() && provider)
        overlay = provider (uiData.overlay);

    // Per-mode top crop: trim `cropTop` design-px off the top. Shrink the height,
    // shift every element up by the same amount, and remember the fraction so paint()
    // draws only the lower part of the background. Everything downstream (resized()
    // scaling, widget rects) then works against the cropped geometry automatically.
    if (const int crop = juce::jlimit (0, juce::jmax (0, uiData.height - 1), uiData.cropTop);
        crop > 0 && uiData.height > 0)
    {
        bgCropFrac = (float) crop / (float) uiData.height;
        uiData.height -= crop;
        for (auto& t : uiData.tabs)
        {
            for (auto& c  : t.controls) c.rect.y  -= crop;
            for (auto& b  : t.buttons)  b.rect.y  -= crop;
            for (auto& im : t.images)   im.rect.y -= crop;
            for (auto& mn : t.menus)    mn.rect.y -= crop;
        }
        if (uiData.strumSpeedReadout)
            uiData.strumSpeedReadout->y -= crop;
    }

    if (uiData.strumSpeedReadout)
    {
        strumSpeedLabel = std::make_unique<juce::Label>();
        strumSpeedLabel->setJustificationType (juce::Justification::centred);
        strumSpeedLabel->setColour (juce::Label::textColourId, juce::Colours::white);
        strumSpeedLabel->setInterceptsMouseClicks (false, false);   // right-click passes through
        addAndMakeVisible (*strumSpeedLabel);
    }

    if (uiData.tabs.isEmpty())
        return;

    auto& tab = uiData.tabs.getReference (0);   // Omni-84 has a single "main" tab

    // Every widget lands in `widgets` (build order = paint order); byIndex/byId let
    // bindings target any widget by document index or element id.
    auto registerWidget = [this] (Widget&& w)
    {
        const int idx = (int) widgets.size();
        if (w.controlIndex >= 0)
            byIndex[w.controlIndex] = idx;
        widgets.push_back (std::move (w));
        return idx;
    };
    auto registerId = [this] (const juce::String& id, int idx)
    {
        if (id.isNotEmpty())
            byId[id] = idx;
    };

    // Lights first so the knobs/buttons paint over them if they overlap.
    for (auto& im : tab.images)
    {
        auto light = std::make_unique<SwappableImage>();
        if (provider) light->setImage (provider (im.image));
        addAndMakeVisible (*light);
        light->setVisible (im.visible);   // authored default (e.g. a patch dialog starts hidden)

        Widget w;
        w.kind = Widget::Kind::light;
        w.comp = std::move (light);
        w.rect = im.rect;
        w.controlIndex = im.controlIndex.value_or (-1);
        registerId (im.id, registerWidget (std::move (w)));
    }

    for (int i = 0; i < tab.controls.size(); ++i)
    {
        auto& c = tab.controls.getReference (i);
        if (! c.skin.has_value())
            continue;   // only filmstrip-skinned controls are knobs

        auto* knob = new FilmstripKnob (provider ? provider (c.skin->image) : juce::Image(),
                                        c.skin->numFrames.value_or (1),
                                        c.min.value_or (0.0), c.max.value_or (1.0),
                                        c.value.value_or (c.min.value_or (0.0)),
                                        c.style.contains ("horizontal"),    // custom_skin_horizontal_drag → drag L/R
                                        c.mouseDragSensitivity.value_or (100.0) < 0.0);   // DS: negative = inverted drag
        const Control* cp = &c;
        knob->onChange = [this, cp] (double v)
        {
            if (onControlChanged) onControlChanged (*cp, v);
            applyVisibilityBindings (*cp, v);   // drive any LED segment images
        };
        knob->onShowValue = [this] (juce::Component& k, const juce::String& t) { showValueBubble (k, t); };
        knob->onRightClick = [this, cp, knob] { if (onControlRightClick) onControlRightClick (*cp, *knob); };
        knob->onHideValue = [this] { if (valueBubble) valueBubble->setVisible (false); };
        addAndMakeVisible (knob);
        knob->setVisible (c.visible);

        Widget w;
        w.kind = Widget::Kind::knob;
        w.comp.reset (knob);
        w.rect = c.rect;
        w.controlIndex = c.controlIndex.value_or (-1);
        w.control = cp;
        registerId (c.id, registerWidget (std::move (w)));
    }

    for (int i = 0; i < tab.buttons.size(); ++i)
    {
        auto& b = tab.buttons.getReference (i);
        std::vector<juce::Image> imgs;
        for (auto& st : b.states)
            imgs.push_back (provider ? provider (st.mainImage) : juce::Image());

        auto* btn = new ImageStateButton (std::move (imgs), b.value.value_or (0));
        const Button* bp = &b;
        const int bi = i;
        btn->onChange = [this, bp, bi] (int s) { handleButton (*bp, bi, s); };
        btn->onRightClick = [this, bp, bi, btn] { if (onButtonRightClick) onButtonRightClick (*bp, bi, *btn); };
        addAndMakeVisible (btn);
        btn->setVisible (b.visible);

        Widget w;
        w.kind = Widget::Kind::button;
        w.comp.reset (btn);
        w.rect = b.rect;
        w.controlIndex = b.controlIndex.value_or (-1);
        w.button = bp;
        w.buttonIndex = bi;
        registerId (b.id, registerWidget (std::move (w)));
    }

    // Initialise each indicator light from its button's default state (not the
    // <image>'s authored picture), so the light matches the button on load.
    for (int i = 0; i < tab.buttons.size(); ++i)
    {
        auto& b = tab.buttons.getReference (i);
        const int s = b.value.value_or (0);
        if (s >= 0 && s < b.states.size())
            applyLightBindings (b.states.getReference (s));
    }

    for (int i = 0; i < tab.menus.size(); ++i)
    {
        auto& m = tab.menus.getReference (i);
        auto* combo = new juce::ComboBox();
        // Manifest-driven styling; defaults reproduce the transparent, left-aligned
        // overlay used by Omni-84 (so unstyled menus look exactly as before).
        auto argb = [] (const juce::String& s)
        {
            auto c = juce::Colour ((juce::uint32) s.getHexValue32());
            return c.getAlpha() == 0 ? c.withAlpha (1.0f) : c;   // 6-digit (no alpha) → opaque
        };
        combo->setColour (juce::ComboBox::backgroundColourId,
                          m.backgroundColor.isNotEmpty() ? argb (m.backgroundColor) : juce::Colours::transparentBlack);
        combo->setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        if (m.textColor.isNotEmpty())
            combo->setColour (juce::ComboBox::textColourId, argb (m.textColor));
        combo->setJustificationType (m.hAlign == "center" ? juce::Justification::centred
                                   : m.hAlign == "right"  ? juce::Justification::centredRight
                                                          : juce::Justification::centredLeft);
        for (int o = 0; o < m.options.size(); ++o)
            combo->addItem (m.options.getReference (o).name, o + 1);
        combo->setSelectedId (juce::jlimit (1, juce::jmax (1, m.options.size()), m.value),
                              juce::dontSendNotification);
        const Menu* mp = &m;
        combo->onChange = [this, mp, combo]
        {
            const int idx = combo->getSelectedId() - 1;
            if (onMenuChanged) onMenuChanged (*mp, idx);
            // A menu option can set many control VALUEs (e.g. selecting a patch loads
            // its 85 drawbar/ADSR/source values). Applied UI-side → each drives its
            // param/engine via the normal control path.
            if (idx >= 0 && idx < mp->options.size())
                applyValueBindings (mp->options.getReference (idx).bindings);
        };
        addAndMakeVisible (combo);
        combo->setVisible (m.visible);

        Widget w;
        w.kind = Widget::Kind::menu;
        w.comp.reset (combo);
        w.rect = m.rect;
        w.controlIndex = m.controlIndex.value_or (-1);
        w.menu = mp;
        registerId (m.id, registerWidget (std::move (w)));
    }

    // Apply each button's initial-state VISIBLE/OPACITY bindings now that every
    // widget exists (a button may show/hide knobs/menus, not just lights). This runs
    // after the per-widget `visible` defaults above, so a button state overrides them
    // — e.g. EDB-Orgel's MIX/MOD toggle hides the modulation bank on load.
    for (int i = 0; i < tab.buttons.size(); ++i)
    {
        const auto& b = tab.buttons.getReference (i);
        const int s = b.value.value_or (0);
        if (s >= 0 && s < b.states.size())
            applyStateVisibility (b.states.getReference (s));
    }

    // Set the initial visibility/opacity of any binding-driven images (so the LED
    // displays start matching their selector instead of all segments stacked on).
    applyAllVisibility();

    // Restore DecentSampler's document z-order (later-declared elements paint on top,
    // e.g. a patch-dialog overlay above the main controls). Only when the manifest
    // carries controlIndex on every widget — i.e. regenerated with the converter that
    // stamps all elements. Legacy manifests (index on images only) keep the build
    // order (lights at back), so the 7 shipped plugins are unaffected until regen.
    const bool docOrdered =
        std::any_of (tab.controls.begin(), tab.controls.end(), [] (const Control& c) { return c.controlIndex.has_value(); })
     || std::any_of (tab.buttons.begin(),  tab.buttons.end(),  [] (const Button&  b) { return b.controlIndex.has_value(); })
     || std::any_of (tab.menus.begin(),    tab.menus.end(),    [] (const Menu&    m) { return m.controlIndex.has_value(); });
    if (docOrdered)
    {
        std::vector<std::pair<int, juce::Component*>> z;
        for (const auto& w : widgets)
            if (w.controlIndex >= 0)
                z.push_back ({ w.controlIndex, w.comp.get() });
        std::sort (z.begin(), z.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& [ci, comp] : z) comp->toFront (false);   // ascending → highest index ends up frontmost
    }

    // Floating value readout (hidden until a knob is turned). Added last so it sits
    // above every widget; non-interactive so it never eats mouse events.
    valueBubble = std::make_unique<juce::Label>();
    valueBubble->setJustificationType (juce::Justification::centred);
    valueBubble->setColour (juce::Label::backgroundColourId, juce::Colour (0xff202122));
    valueBubble->setColour (juce::Label::textColourId,       juce::Colour (0xffffffff));
    valueBubble->setColour (juce::Label::outlineColourId,    juce::Colour (0x40ffffff));
    valueBubble->setInterceptsMouseClicks (false, false);
    valueBubble->setVisible (false);
    addChildComponent (valueBubble.get());
}

void ManifestUiComponent::showValueBubble (juce::Component& knob, const juce::String& text)
{
    if (valueBubble == nullptr)
        return;

    valueBubble->setText (text, juce::dontSendNotification);
    valueBubble->setFont (juce::Font (juce::FontOptions().withHeight (13.0f)));

    const int w = juce::jmax (34, text.length() * 9 + 12);   // rough width for a short number
    const int h = 19;
    const auto kb = knob.getBounds();
    int x = kb.getCentreX() - w / 2;
    int y = kb.getY() - h - 2;                 // just above the knob
    if (y < 0) y = kb.getBottom() + 2;          // no room above → put it below
    x = juce::jlimit (0, juce::jmax (0, getWidth() - w), x);

    valueBubble->setBounds (x, y, w, h);
    valueBubble->setVisible (true);
    valueBubble->toFront (false);
}

void ManifestUiComponent::refresh (
    const std::function<std::optional<double> (const Control&)>&     controlValue,
    const std::function<std::optional<int> (const Button&, int idx)>& buttonState,
    const std::function<std::optional<int> (const Menu&)>&            menuSelection)
{
    for (auto& w : widgets)
    {
        if (w.kind == Widget::Kind::knob)
        {
            auto* knob = static_cast<FilmstripKnob*> (w.comp.get());
            if (auto v = controlValue (*w.control))
                if (! juce::exactlyEqual (*v, knob->getValue()))   // only on change — setValue repaints, and the
                {                                                  // visibility re-eval re-parses translation tables
                    knob->setValue (*v);
                    applyVisibilityBindings (*w.control, *v);      // this knob may be an LED selector
                }
        }
        else if (w.kind == Widget::Kind::button)
        {
            auto* btn = static_cast<ImageStateButton*> (w.comp.get());
            if (auto s = buttonState (*w.button, w.buttonIndex))
                if (*s != btn->getState())   // only on change (re-decoding light PNGs is costly)
                {
                    btn->setState (*s);
                    if (*s >= 0 && *s < w.button->states.size())
                    {
                        applyLightBindings   (w.button->states.getReference (*s));   // keep the paired light in sync
                        applyStateVisibility (w.button->states.getReference (*s));   // keep target widgets in sync
                    }
                }
        }
        else if (w.kind == Widget::Kind::menu)
        {
            auto* combo = static_cast<juce::ComboBox*> (w.comp.get());
            if (auto sel = menuSelection (*w.menu))
                if (combo->getSelectedId() != *sel + 1)
                    combo->setSelectedId (*sel + 1, juce::dontSendNotification);
        }
    }

    // No blanket applyAllVisibility() here: the initial state is established when the UI
    // is built (rebuild calls it once), and knobs (above) + buttons resync their targets
    // exactly when a value changes — re-walking every binding (and re-parsing its
    // translation table) at the full timer rate was pure waste.
}

int ManifestUiComponent::widgetForBinding (const Binding& b) const
{
    if (b.targetId.isNotEmpty())
        if (auto it = byId.find (b.targetId); it != byId.end())
            return it->second;
    if (b.controlIndex)
        if (auto it = byIndex.find (*b.controlIndex); it != byIndex.end())
            return it->second;
    return -1;
}

void ManifestUiComponent::applyVisibilityBinding (const Binding& b, double sourceValue)
{
    if (b.type != "control")
        return;
    const bool isOpacity = b.parameter == "OPACITY";
    if (! isOpacity && b.parameter != "VISIBLE")
        return;
    const int wi = widgetForBinding (b);
    if (wi < 0)
        return;

    // Output value: a translation table maps the source (knob) value; otherwise a
    // fixed translationValue (a button state's bool/number); otherwise the raw source.
    double out = sourceValue;
    if (b.translationTable.isNotEmpty())
        out = evalTableLinear (b.translationTable, sourceValue);
    else if (b.translationValue.isBool())
        out = ((bool) b.translationValue) ? 1.0 : 0.0;
    else if (b.translationValue.isDouble() || b.translationValue.isInt())
        out = (double) b.translationValue;

    auto& w = widgets[(size_t) wi];
    if (w.kind == Widget::Kind::light)
    {
        // Lights keep their dedicated path (image-alpha crossfade for OPACITY).
        auto* light = static_cast<SwappableImage*> (w.comp.get());
        if (isOpacity) light->setImageAlpha ((float) out);
        else           light->setVisible (out > 0.5);
    }
    else
    {
        if (isOpacity) w.comp->setAlpha ((float) out);
        else           w.comp->setVisible (out > 0.5);
    }
}

void ManifestUiComponent::applyVisibilityBindings (const Control& c, double value)
{
    for (const auto& b : c.bindings)
        applyVisibilityBinding (b, value);
}

void ManifestUiComponent::applyStateVisibility (const ButtonState& state)
{
    for (const auto& b : state.bindings)
        applyVisibilityBinding (b, 0.0);   // value unused: button states carry a fixed translationValue
}

void ManifestUiComponent::applyValueBindings (const juce::Array<Binding>& bindings)
{
    for (const auto& b : bindings)
        if (b.type == "control" && b.parameter == "VALUE")
        {
            const int wi = widgetForBinding (b);
            if (wi < 0)
                continue;
            const double v = b.translationValue.isBool()
                               ? (((bool) b.translationValue) ? 1.0 : 0.0)
                               : b.translationValue.toString().getDoubleValue();
            setWidgetValue (wi, v);
        }
}

void ManifestUiComponent::setWidgetValue (int widgetIdx, double value)
{
    if (widgetIdx < 0 || widgetIdx >= (int) widgets.size())
        return;
    auto& w = widgets[(size_t) widgetIdx];

    // Only act on an actual change — this both avoids redundant work and terminates
    // the button↔button cascade (e.g. the patch dialog's load/close pair).
    if (w.kind == Widget::Kind::button)
    {
        auto* btn = static_cast<ImageStateButton*> (w.comp.get());
        const int s = (int) value;
        if (s != btn->getState())
        {
            btn->setState (s);
            handleButton (*w.button, w.buttonIndex, s);   // fires onButtonChanged (APVTS sync) + cascades
        }
    }
    else if (w.kind == Widget::Kind::knob)
    {
        auto* knob = static_cast<FilmstripKnob*> (w.comp.get());
        if (! juce::exactlyEqual (value, knob->getValue()))
        {
            knob->setValue (value);
            if (onControlChanged) onControlChanged (*w.control, value);
            applyVisibilityBindings (*w.control, value);
        }
    }
    else if (w.kind == Widget::Kind::menu)
    {
        auto* combo = static_cast<juce::ComboBox*> (w.comp.get());
        const int sel = (int) value;
        if (combo->getSelectedId() != sel + 1)
            combo->setSelectedId (sel + 1, juce::sendNotification);   // fires onMenuChanged
    }
}

void ManifestUiComponent::applyAllVisibility()
{
    for (auto& w : widgets)
        if (w.kind == Widget::Kind::knob)
            applyVisibilityBindings (*w.control,
                                     static_cast<FilmstripKnob*> (w.comp.get())->getValue());
}

ManifestUiComponent::~ManifestUiComponent() = default;

void ManifestUiComponent::handleButton (const Button& b, int index, int stateIndex)
{
    if (stateIndex >= 0 && stateIndex < b.states.size())
    {
        applyLightBindings    (b.states.getReference (stateIndex));            // image swaps
        applyStateVisibility  (b.states.getReference (stateIndex));            // show/hide target widgets
        applyValueBindings    (b.states.getReference (stateIndex).bindings);   // set other widgets' values (cascade)
    }

    if (onButtonChanged)
        onButtonChanged (b, index, stateIndex);
}

void ManifestUiComponent::applyLightBindings (const ButtonState& state)
{
    // A button state's PATH bindings each address a light by its controlIndex
    // (document-order UI index) — one button may drive several lights (e.g. the
    // Mono/Poly switch lights an upper or lower lamp), so match by index, not pairing.
    if (! provider)
        return;

    for (const auto& bind : state.bindings)
    {
        if (bind.parameter != "PATH" || ! bind.translationValue.isString())
            continue;

        const int wi = widgetForBinding (bind);
        if (wi >= 0 && widgets[(size_t) wi].kind == Widget::Kind::light)
            static_cast<SwappableImage*> (widgets[(size_t) wi].comp.get())
                ->setImage (provider (bind.translationValue.toString()));
    }
}

void ManifestUiComponent::paint (juce::Graphics& g)
{
    if (background.isValid())
    {
        const int imgW = background.getWidth();
        const int imgH = background.getHeight();
        const int srcY = juce::jlimit (0, juce::jmax (0, imgH - 1), juce::roundToInt (bgCropFrac * (float) imgH));
        g.drawImage (background,
                     0, 0, getWidth(), getHeight(),   // dest: the whole (cropped) component
                     0, srcY, imgW, imgH - srcY);      // src: lower part of the background only
    }
    else
        g.fillAll (juce::Colour (0xff222222));
}

void ManifestUiComponent::paintOverChildren (juce::Graphics& g)
{
    // The overlay is drawn AFTER every child component, so it sits on top of the
    // background and all controls. It is pure painting — not a component — so it never
    // intercepts mouse events; the controls underneath stay fully interactive. Uses the
    // same stretch + top-crop transform as the background so it stays aligned at any size.
    if (! overlay.isValid())
        return;

    const int imgW = overlay.getWidth();
    const int imgH = overlay.getHeight();
    const int srcY = juce::jlimit (0, juce::jmax (0, imgH - 1), juce::roundToInt (bgCropFrac * (float) imgH));
    g.drawImage (overlay,
                 0, 0, getWidth(), getHeight(),   // dest: the whole (cropped) component
                 0, srcY, imgW, imgH - srcY);      // src: lower part only, matching the background crop
}

void ManifestUiComponent::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onBackgroundRightClick)
        onBackgroundRightClick();
}

void ManifestUiComponent::resized()
{
    // The background is stretched to fill the component, so scale the widget
    // positions by the same ratio (design space = ui.width × ui.height) — keeps
    // knobs/buttons aligned to the background at any component size.
    const double sx = uiData.width  > 0 ? (double) getWidth()  / (double) uiData.width  : 1.0;
    const double sy = uiData.height > 0 ? (double) getHeight() / (double) uiData.height : 1.0;

    auto place = [&] (juce::Component& c, const Rect& r)
    {
        c.setBounds (juce::roundToInt (r.x * sx), juce::roundToInt (r.y * sy),
                     juce::roundToInt (r.width * sx), juce::roundToInt (r.height * sy));
    };

    for (auto& w : widgets)
        place (*w.comp, w.rect);

    if (strumSpeedLabel != nullptr && uiData.strumSpeedReadout)
    {
        place (*strumSpeedLabel, *uiData.strumSpeedReadout);
        strumSpeedLabel->setFont (juce::FontOptions ((float) strumSpeedLabel->getHeight() * 0.72f));
    }
}

void ManifestUiComponent::setStrumSpeedText (const juce::String& text)
{
    if (strumSpeedLabel != nullptr)
        strumSpeedLabel->setText (text, juce::dontSendNotification);
}

} // namespace dm
