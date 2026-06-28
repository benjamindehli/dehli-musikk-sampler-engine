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
    /** A button changed to `stateIndex`. (Light swaps are handled internally.) */
    std::function<void (const Button&, int stateIndex)> onButtonChanged;
    /** A dropdown menu selected `optionIndex` (0-based). */
    std::function<void (const Menu&, int optionIndex)> onMenuChanged;

    /** Push externally-held values (e.g. host automation / restored params) into the
        widgets WITHOUT firing the change callbacks. Each callback is asked, per
        widget model, what it should display; std::nullopt leaves it untouched. The
        renderer stays plugin-agnostic — the host maps params to controls. */
    void refresh (const std::function<std::optional<double> (const Control&)>& controlValue,
                  const std::function<std::optional<int>    (const Button&)>&  buttonState,
                  const std::function<std::optional<int>    (const Menu&)>&    menuSelection);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class FilmstripKnob;
    class ImageStateButton;
    class SwappableImage;

    void handleButton (const Button& b, int index, int stateIndex);
    void applyLightBindings (const ButtonState& state);   // PATH swaps, addressed by controlIndex

    Ui uiData;                          // owned copy (widgets reference it)
    ImageProvider provider;
    juce::Image background;

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestUiComponent)
};

} // namespace dm
