#pragma once

// dehli-musikk-sampler-engine — data-driven UI renderer (M4).
//
// Builds the plugin face from a mode's Ui tree: background image, filmstrip knobs
// (custom_skin_vertical_drag), image buttons with state swaps, and indicator
// lights. It is engine-agnostic — control/button changes are reported via
// callbacks; the host plugin maps them to engine parameters through the bindings.
//
// Images are supplied by an ImageProvider (id → juce::Image), so the renderer
// doesn't care where they live (the plugin loads them from embedded BinaryData).

#include <model/Manifest.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>
#include <memory>
#include <map>
#include <vector>

namespace dm
{

class ManifestUiComponent : public juce::Component
{
public:
    using ImageProvider = std::function<juce::Image (const juce::String&)>;

    ManifestUiComponent (const Ui& ui, ImageProvider provider);
    ~ManifestUiComponent() override;

    /** A control (knob) moved — `value` is in the control's own min..max range. */
    std::function<void (const Control&, double value)> onControlChanged;
    /** A button (its index in the tab) changed to `stateIndex`. (Lights handled internally.) */
    std::function<void (const Button&, int buttonIndex, int stateIndex)> onButtonChanged;
    /** A dropdown menu selected `optionIndex` (0-based). */
    std::function<void (const Menu&, int optionIndex)> onMenuChanged;

    // Right-click (context menu) hooks: on a knob/button widget → the editor shows
    // MIDI Learn etc.; on the background → the editor shows the plain menu.
    std::function<void (const Control&, juce::Component&)> onControlRightClick;
    std::function<void (const Button&, int buttonIndex, juce::Component&)> onButtonRightClick;
    std::function<void()> onBackgroundRightClick;

    /** Push externally-held values (e.g. host automation / restored params) into the
        widgets WITHOUT firing the change callbacks. Each callback is asked, per
        widget model, what it should display; std::nullopt leaves it untouched. The
        renderer stays plugin-agnostic — the host maps params to controls. */
    void refresh (const std::function<std::optional<double> (const Control&)>&     controlValue,
                  const std::function<std::optional<int> (const Button&, int idx)>& buttonState,
                  const std::function<std::optional<int> (const Menu&)>&            menuSelection);

    /** Live strum-speed readout text (mode has ui.strumSpeedReadout): white text over
        the ribbon painted in the background. No-op when the mode has no readout. */
    void setStrumSpeedText (const juce::String& text);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // overlay image, drawn over every control
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;   // background right-click → context menu

private:
    class FilmstripKnob;
    class ImageStateButton;
    class SwappableImage;

    void handleButton (const Button& b, int index, int stateIndex);
    void applyLightBindings (const ButtonState& state);   // PATH swaps, addressed by controlIndex

    // Floating value readout shown next to a knob while it is being turned.
    void showValueBubble (juce::Component& knob, const juce::String& text);
    std::unique_ptr<juce::Label> valueBubble;

    // Strum-speed readout (ui.strumSpeedReadout): transparent centred label the
    // editor feeds via setStrumSpeedText each tick.
    std::unique_ptr<juce::Label> strumSpeedLabel;

    // One record per rendered widget, in build order (lights, knobs, buttons, menus —
    // same as before, so painting/z-order defaults are unchanged). Replaces the former
    // FOUR parallel array clusters (comp/rect/model per widget type) plus their five
    // controlIndex/id maps: every operation — resize, refresh, visibility, value
    // cascades, z-order — now walks or indexes this single list.
    struct Widget
    {
        enum class Kind { light, knob, button, menu };
        Kind kind { Kind::light };
        std::unique_ptr<juce::Component> comp;
        Rect rect;
        int controlIndex = -1;               // document order (-1 = none)
        const Control* control = nullptr;    // kind == knob
        const Button*  button  = nullptr;    // kind == button
        const Menu*    menu    = nullptr;    // kind == menu
        int buttonIndex = -1;                // kind == button: index within the tab's buttons
    };
    std::vector<Widget> widgets;
    std::map<int, int>          byIndex;     // controlIndex → widgets[] index
    std::map<juce::String, int> byId;        // element id   → widgets[] index

    // VISIBLE/OPACITY/VALUE/PATH: a source (a control's value, a button state, or a
    // menu option) targets another widget by id (preferred) or legacy controlIndex —
    // e.g. LED segments, EDB-Orgel's MIX/MOD toggle hiding banks of controls, the
    // patch dialog. Resolve to a widgets[] index, or -1.
    int  widgetForBinding (const Binding& b) const;

    void applyVisibilityBinding  (const Binding& b, double sourceValue);
    void applyVisibilityBindings (const Control& c, double value);      // knob-driven
    void applyStateVisibility    (const ButtonState& state);            // button-driven
    void applyAllVisibility();   // re-evaluate every source control (load + refresh)

    // VALUE bindings: a source (a button state, or a selected menu option) sets OTHER
    // widgets' values. Powers the patch dialog's two-button cross-toggle AND patch
    // loading (a menu option sets 85 drawbar/ADSR/source controls).
    void applyValueBindings (const juce::Array<Binding>& bindings);
    void setWidgetValue     (int widgetIdx, double value);

    Ui uiData;                          // owned copy (widgets reference it; height/rects
                                        // already adjusted for cropTop in the ctor)
    ImageProvider provider;
    juce::Image background;
    juce::Image overlay;                // optional, drawn over the background AND all controls
    float bgCropFrac { 0.0f };          // fraction of the background trimmed off the top

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestUiComponent)
};

} // namespace dm
