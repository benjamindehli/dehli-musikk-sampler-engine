#pragma once

// Flat monochrome window chrome for the Standalone build: a dark (#202122)
// title bar + body with white (#ffffff) title text and button glyphs that invert
// on hover/press. ManifestEditor attaches this to the standalone DocumentWindow;
// inside a DAW there is no DocumentWindow parent, so it is never applied there.

#include <juce_gui_basics/juce_gui_basics.h>

namespace dm
{

class StandaloneWindowLookAndFeel : public juce::LookAndFeel_V4
{
public:
    static constexpr juce::uint32 kBackground = 0xff202122;
    static constexpr juce::uint32 kForeground = 0xffffffff;

    juce::Button* createDocumentWindowButton (int buttonType) override
    {
        juce::Path shape;
        constexpr float t = 0.15f;   // glyph stroke thickness (unit square)

        if (buttonType == juce::DocumentWindow::closeButton)
        {
            shape.addLineSegment ({ 0.0f, 0.0f, 1.0f, 1.0f }, t);
            shape.addLineSegment ({ 1.0f, 0.0f, 0.0f, 1.0f }, t);
        }
        else if (buttonType == juce::DocumentWindow::minimiseButton)
        {
            shape.addLineSegment ({ 0.0f, 0.5f, 1.0f, 0.5f }, t);
        }
        else   // maximiseButton
        {
            shape.addLineSegment ({ 0.5f, 0.0f, 0.5f, 1.0f }, t);
            shape.addLineSegment ({ 0.0f, 0.5f, 1.0f, 0.5f }, t);
        }

        return new GlyphButton (shape);
    }

    void drawDocumentWindowTitleBar (juce::DocumentWindow& window, juce::Graphics& g,
                                     int w, int h, int titleSpaceX, int titleSpaceW,
                                     const juce::Image*, bool) override
    {
        g.fillAll (juce::Colour (kBackground));
        g.setColour (juce::Colour (kForeground));
        g.setFont (juce::Font (juce::FontOptions().withHeight ((float) h * 0.5f)));
        g.drawText (window.getName(), titleSpaceX, 0, titleSpaceW, h,
                    juce::Justification::centred, true);
        juce::ignoreUnused (w);
    }

private:
    // White glyph on the dark window background; fills white and draws the glyph in
    // the background colour when highlighted/pressed (a clean monochrome invert).
    struct GlyphButton : juce::Button
    {
        explicit GlyphButton (juce::Path s) : juce::Button ({}), shape (std::move (s)) {}

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto bg = juce::Colour (kBackground);
            const auto fg = juce::Colour (kForeground);

            g.fillAll (bg);
            auto glyphColour = fg;
            if (highlighted || down)
            {
                g.setColour (down ? fg.withAlpha (0.7f) : fg);
                g.fillAll();
                glyphColour = bg;
            }

            g.setColour (glyphColour);
            auto r = getLocalBounds().toFloat().reduced ((float) getHeight() * 0.32f);
            g.fillPath (shape, shape.getTransformToScaleToFit (r, true));
        }

        juce::Path shape;
    };
};

} // namespace dm
