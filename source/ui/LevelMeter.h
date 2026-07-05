#pragma once

// dehli-musikk-sampler-engine — compact output peak meter (dBFS) with a latching
// clip indicator. Plugin-agnostic: the editor feeds it the output peak each timer
// tick (peak since the last read), it shows a held/decaying bar + a clip LED that
// stays lit until clicked. Lets you see clipping even in the Standalone.

#include <juce_gui_basics/juce_gui_basics.h>

namespace dm
{

class LevelMeter : public juce::Component
{
public:
    LevelMeter() { setInterceptsMouseClicks (true, false); }

    /** Stereo peaks (max |sample| per channel over the block(s) since the last call). */
    void setLevels (float peakL, float peakR)
    {
        updateChannel (displayDbL, peakL);
        updateChannel (displayDbR, peakR);
        if (peakL >= 1.0f || peakR >= 1.0f) clipped = true;   // latch clip (either channel)
        repaint();
    }

    /** Mono convenience — drives both channels the same. */
    void setLevel (float peakLinear) { setLevels (peakLinear, peakLinear); }

    void mouseDown (const juce::MouseEvent&) override { clipped = false; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const float clipW = juce::jmin (12.0f, r.getWidth() * 0.18f);
        auto clipArea = r.removeFromRight (clipW);
        r.removeFromRight (2.0f);

        // Split the height into two stacked horizontal bars: L on top, R below.
        const float gap = 1.0f;
        const float barH = (r.getHeight() - gap) * 0.5f;
        auto lArea = r.removeFromTop (barH);
        r.removeFromTop (gap);
        drawBar (g, lArea, displayDbL);
        drawBar (g, r,     displayDbR);

        g.setColour (clipped ? juce::Colours::red : juce::Colour (0xff3a2222));
        g.fillRoundedRectangle (clipArea, 2.0f);
    }

private:
    static void updateChannel (float& displayDb, float peakLinear)
    {
        const float db = peakLinear > 1.0e-6f ? juce::Decibels::gainToDecibels (peakLinear) : -120.0f;
        if (db >= displayDb) displayDb = db;                              // instant attack
        else                 displayDb = juce::jmax (db, displayDb - 2.0f); // ~2 dB/tick decay
    }

    static void drawBar (juce::Graphics& g, juce::Rectangle<float> area, float displayDb)
    {
        g.setColour (juce::Colour (0xff1b1b1b));
        g.fillRoundedRectangle (area, 1.5f);

        constexpr float kMinDb = -48.0f;
        const float norm = juce::jlimit (0.0f, 1.0f, (displayDb - kMinDb) / (0.0f - kMinDb));
        if (norm > 0.0f)
        {
            const juce::Colour c = displayDb > -3.0f  ? juce::Colours::red
                                 : displayDb > -12.0f ? juce::Colour (0xffe6c100)   // amber
                                                      : juce::Colour (0xff39d353);  // green
            g.setColour (c);
            g.fillRoundedRectangle (area.withWidth (area.getWidth() * norm), 1.5f);
        }
    }

    float displayDbL { -120.0f }, displayDbR { -120.0f };
    bool  clipped { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

} // namespace dm
