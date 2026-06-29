#pragma once

// Reusable look for pitch/mod wheels: vertical sliders drawn as little wheels —
// a recessed slot, a shaded face, and a bright ridge marking the position.
// Shared across plugins (lives in the engine to avoid per-plugin duplication).

#include <juce_gui_basics/juce_gui_basics.h>

namespace dm
{

class WheelLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // Use the full component bounds — the default layout insets a vertical slider by
    // the thumb radius (top+bottom), which made the wheel shorter than the keyboard.
    juce::Slider::SliderLayout getSliderLayout (juce::Slider& slider) override
    {
        juce::Slider::SliderLayout layout;
        layout.sliderBounds = slider.getLocalBounds();
        return layout;
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override
    {
        auto slot = juce::Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (0.5f);
        const float r = 3.0f;

        g.setColour (juce::Colour (0xffa8a8a8));                       // slot/border (neutral gray)
        g.fillRoundedRectangle (slot, r);

        auto face = slot.reduced (1.0f);                               // neutral gray, centre #d3d3d3 (like the keyboard arrows)
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe3e3e3), face.getCentreX(), face.getY(),
                                                 juce::Colour (0xffc3c3c3), face.getCentreX(), face.getBottom(), false));
        g.fillRoundedRectangle (face, r);

        const float ty = juce::jlimit (face.getY() + 1.0f, face.getBottom() - 1.0f, sliderPos);
        g.setColour (juce::Colour (0xff4a4a4a));                       // position ridge (neutral dark)
        g.fillRect (face.getX(), ty - 1.0f, face.getWidth(), 2.0f);

        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.drawRoundedRectangle (slot, r, 1.0f);
    }
};

} // namespace dm
