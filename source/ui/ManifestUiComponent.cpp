#include "ManifestUiComponent.h"
#include <cmath>

namespace dm
{

// ---------------------------------------------------------------------------
// Filmstrip rotary knob: a vertical strip of `frames` images; the frame shown is
// chosen by the normalised value. Vertical drag changes the value.
// ---------------------------------------------------------------------------
class ManifestUiComponent::FilmstripKnob : public juce::Component
{
public:
    FilmstripKnob (juce::Image strip, int numFrames, double minV, double maxV, double initial)
        : film (strip), frames (juce::jmax (1, numFrames)), minVal (minV), maxVal (maxV)
    {
        value = juce::jlimit (minVal, maxVal, initial);
    }

    std::function<void (double)> onChange;

    double getValue() const noexcept { return value; }

    /** Set the displayed value WITHOUT firing onChange (used for external sync). */
    void setValue (double v)
    {
        const double nv = juce::jlimit (minVal, maxVal, v);
        if (nv != value) { value = nv; repaint(); }
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
        dragStartY = e.position.y;
        dragStartVal = value;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const double range = maxVal - minVal;
        if (range <= 0.0)
            return;

        const double dy = (double) (dragStartY - e.position.y);     // drag up = increase
        double norm = (dragStartVal - minVal) / range + dy / 200.0; // 200 px = full sweep
        value = minVal + juce::jlimit (0.0, 1.0, norm) * range;
        repaint();
        if (onChange)
            onChange (value);
    }

private:
    juce::Image film;
    int frames;
    double minVal, maxVal, value { 0.0 };
    float dragStartY { 0.0f };
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

    void mouseDown (const juce::MouseEvent&) override
    {
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

    void paint (juce::Graphics& g) override
    {
        if (image.isValid())
            g.drawImage (image, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
    }

private:
    juce::Image image;
};

// ---------------------------------------------------------------------------

ManifestUiComponent::ManifestUiComponent (const Ui& ui, ImageProvider imageProvider)
    : uiData (ui), provider (std::move (imageProvider))
{
    if (uiData.background.isNotEmpty() && provider)
        background = provider (uiData.background);

    if (uiData.tabs.isEmpty())
        return;

    auto& tab = uiData.tabs.getReference (0);   // Omni-84 has a single "main" tab

    // Lights first so the knobs/buttons paint over them if they overlap.
    for (auto& im : tab.images)
    {
        auto* light = new SwappableImage();
        if (provider) light->setImage (provider (im.image));
        addAndMakeVisible (light);
        lights.add (light);
        lightRects.add (im.rect);
        lightControlIndex.add (im.controlIndex.value_or (-1));
    }

    for (int i = 0; i < tab.controls.size(); ++i)
    {
        auto& c = tab.controls.getReference (i);
        if (! c.skin.has_value())
            continue;   // only filmstrip-skinned controls are knobs

        auto* knob = new FilmstripKnob (provider ? provider (c.skin->image) : juce::Image(),
                                        c.skin->numFrames.value_or (1),
                                        c.min.value_or (0.0), c.max.value_or (1.0),
                                        c.value.value_or (c.min.value_or (0.0)));
        const Control* cp = &c;
        knob->onChange = [this, cp] (double v) { if (onControlChanged) onControlChanged (*cp, v); };
        addAndMakeVisible (knob);
        knobs.add (knob);
        knobRects.add (c.rect);
        knobModels.add (cp);
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
        addAndMakeVisible (btn);
        buttons.add (btn);
        buttonRects.add (b.rect);
        buttonModels.add (bp);
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
        combo->onChange = [this, mp, combo] { if (onMenuChanged) onMenuChanged (*mp, combo->getSelectedId() - 1); };
        addAndMakeVisible (combo);
        menus.add (combo);
        menuRects.add (m.rect);
        menuModels.add (mp);
    }
}

void ManifestUiComponent::refresh (
    const std::function<std::optional<double> (const Control&)>& controlValue,
    const std::function<std::optional<int>    (const Button&)>&  buttonState,
    const std::function<std::optional<int>    (const Menu&)>&    menuSelection)
{
    for (int i = 0; i < knobs.size(); ++i)
        if (auto v = controlValue (*knobModels[i]))
            knobs[i]->setValue (*v);

    for (int i = 0; i < buttons.size(); ++i)
        if (auto s = buttonState (*buttonModels[i]))
            if (*s != buttons[i]->getState())   // only on change (re-decoding light PNGs is costly)
            {
                buttons[i]->setState (*s);
                const auto& b = *buttonModels[i];
                if (*s >= 0 && *s < b.states.size())
                    applyLightBindings (b.states.getReference (*s));   // keep the paired light in sync
            }

    for (int i = 0; i < menus.size(); ++i)
        if (auto sel = menuSelection (*menuModels[i]))
            if (menus[i]->getSelectedId() != *sel + 1)
                menus[i]->setSelectedId (*sel + 1, juce::dontSendNotification);
}

ManifestUiComponent::~ManifestUiComponent() = default;

void ManifestUiComponent::handleButton (const Button& b, int index, int stateIndex)
{
    juce::ignoreUnused (index);
    if (stateIndex >= 0 && stateIndex < b.states.size())
        applyLightBindings (b.states.getReference (stateIndex));

    if (onButtonChanged)
        onButtonChanged (b, stateIndex);
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
        if (bind.parameter != "PATH" || ! bind.translationValue.isString() || ! bind.controlIndex)
            continue;

        const int idx = lightControlIndex.indexOf (*bind.controlIndex);
        if (idx >= 0)
            lights[idx]->setImage (provider (bind.translationValue.toString()));
    }
}

void ManifestUiComponent::paint (juce::Graphics& g)
{
    if (background.isValid())
        g.drawImage (background, getLocalBounds().toFloat(), juce::RectanglePlacement::stretchToFit);
    else
        g.fillAll (juce::Colour (0xff222222));
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

    for (int i = 0; i < lights.size();  ++i) place (*lights[i],  lightRects[i]);
    for (int i = 0; i < knobs.size();   ++i) place (*knobs[i],   knobRects[i]);
    for (int i = 0; i < buttons.size(); ++i) place (*buttons[i], buttonRects[i]);
    for (int i = 0; i < menus.size();   ++i) place (*menus[i],   menuRects[i]);
}

} // namespace dm
