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

    /** Push externally-held values (e.g. host automation / restored params) into the
        widgets WITHOUT firing the change callbacks. Each callback is asked, per
        widget model, what it should display; std::nullopt leaves it untouched. The
        renderer stays plugin-agnostic — the host maps params to controls. */
    void refresh (const std::function<std::optional<double> (const Control&)>&     controlValue,
                  const std::function<std::optional<int> (const Button&, int idx)>& buttonState,
                  const std::function<std::optional<int> (const Menu&)>&            menuSelection);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class FilmstripKnob;
    class ImageStateButton;
    class SwappableImage;

    void handleButton (const Button& b, int index, int stateIndex);
    void applyLightBindings (const ButtonState& state);   // PATH swaps, addressed by controlIndex

    // Floating value readout shown next to a knob while it is being turned.
    void showValueBubble (juce::Component& knob, const juce::String& text);
    std::unique_ptr<juce::Label> valueBubble;

    // VISIBLE/OPACITY: a source (a control's value, or a button state) drives the
    // visibility/alpha of a target widget addressed by controlIndex. Targets can be
    // lights (LED segments — pitch up/down crossfade), OR any knob/button/menu (e.g.
    // EDB-Orgel's MIX/MOD toggle hides/shows whole banks of controls, and the
    // patch-load dialog).
    // Resolve a binding's UI target to a document-order controlIndex: prefer the id
    // (targetId → element id), falling back to the legacy controlIndex. Lets the
    // existing controlIndex-keyed maps stay while bindings become id-based.
    int  controlIndexForBinding (const Binding& b) const;

    void applyVisibilityBinding  (const Binding& b, double sourceValue);
    void applyVisibilityBindings (const Control& c, double value);      // knob-driven
    void applyStateVisibility    (const ButtonState& state);            // button-driven
    void applyAllVisibility();   // re-evaluate every source control (load + refresh)

    // VALUE bindings: a source (a button state, or a selected menu option) sets OTHER
    // widgets' values by controlIndex. Powers the patch dialog's two-button cross-toggle
    // (load icon sets the close button's state → its state drives the dialog's VISIBLE
    // bindings) AND patch loading (a menu option sets 85 drawbar/ADSR/source controls).
    void applyValueBindings (const juce::Array<Binding>& bindings);
    void setWidgetValue     (int controlIndex, double value);

    Ui uiData;                          // owned copy (widgets reference it; height/rects
                                        // already adjusted for cropTop in the ctor)
    ImageProvider provider;
    juce::Image background;
    float bgCropFrac { 0.0f };          // fraction of the background trimmed off the top

    juce::OwnedArray<FilmstripKnob>   knobs;
    juce::Array<Rect>                 knobRects;
    juce::Array<const Control*>       knobModels;          // parallel to knobs (for refresh)
    juce::OwnedArray<ImageStateButton> buttons;
    juce::Array<Rect>                 buttonRects;
    juce::Array<const Button*>        buttonModels;        // parallel to buttons
    juce::OwnedArray<SwappableImage>  lights;
    juce::Array<Rect>                 lightRects;
    juce::Array<int>                  lightControlIndex;   // document-order index per light
    juce::OwnedArray<juce::ComboBox>  menus;
    juce::Array<Rect>                 menuRects;
    juce::Array<const Menu*>          menuModels;          // parallel to menus

    // controlIndex (document order) → widget, for VISIBLE/OPACITY bindings that
    // target knobs/buttons/menus (lights use lightControlIndex above).
    std::map<int, juce::Component*>   widgetByIndex;
    // controlIndex → index into buttons[]/knobs[]/menus[], for VALUE bindings that
    // set another widget's value (need the model + index to fire the callback).
    std::map<int, int>                buttonIdxByCI, knobIdxByCI, menuIdxByCI;
    // element id → controlIndex, so id-based binding targets resolve to the existing
    // controlIndex-keyed maps above.
    std::map<juce::String, int>       idToCI;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestUiComponent)
};

} // namespace dm
