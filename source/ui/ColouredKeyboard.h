#pragma once

// A MidiKeyboardComponent that tints configured note ranges — shows the playable
// zones DecentSampler defines via <keyboard><color .../>, carried in the manifest
// as Ui::keyboardColors (loNote/hiNote/ARGB). Ranges are per-mode, so the editor
// refreshes them on every mode switch. Shared across plugins (lives in the engine).

#include <juce_audio_utils/juce_audio_utils.h>
#include <model/Manifest.h>
#include <optional>

namespace dm
{

class ColouredKeyboard : public juce::MidiKeyboardComponent
{
public:
    using juce::MidiKeyboardComponent::MidiKeyboardComponent;

    void setColourRanges (const juce::Array<KeyboardColor>& ranges)
    {
        parsed.clearQuick();
        for (const auto& r : ranges)
        {
            // "none"/"transparent" → the range is still declared (so the editor keeps
            // those keys visible), but drawn WITHOUT a tint → regular black/white keys.
            if (r.color.trim().equalsIgnoreCase ("none") || r.color.trim().equalsIgnoreCase ("transparent"))
                continue;
            auto colour = juce::Colour ((juce::uint32) r.color.getHexValue32());
            if (colour.getAlpha() == 0)            // 6-digit (no alpha) → treat as opaque
                colour = colour.withAlpha (1.0f);
            parsed.add ({ r.loNote, r.hiNote, colour });
        }
        repaint();
    }

    /** Global per-key-type tint overlaid on ALL white / black keys (independent of the
        note-range zones above). ARGB hex; the alpha sets the strength (e.g. "30ffcc00" =
        subtle yellow). Empty / "none" / alpha-0 = no tint for that key type. */
    void setKeyTints (const juce::String& whiteTint, const juce::String& blackTint)
    {
        whiteKeyTint = parseTint (whiteTint);
        blackKeyTint = parseTint (blackTint);
        repaint();
    }

protected:
    // DecentSampler ignored the file's alpha and tinted keys at a fixed amount, drawn
    // OVER the normal key — so white keys read as light tints and black keys as dark
    // tints of the same hue, with shading + down/over states preserved. We match that
    // by drawing the base key first, then overlaying the colour at these alphas.
    static constexpr float kWhiteTintAlpha = 0.75f;
    static constexpr float kBlackTintAlpha = 0.5f;

    void drawWhiteNote (int note, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour lineColour, juce::Colour textColour) override
    {
        juce::MidiKeyboardComponent::drawWhiteNote (note, g, area, isDown, isOver, lineColour, textColour);
        if (whiteKeyTint)                    // global aesthetic tint (honours its own alpha)
        {
            g.setColour (*whiteKeyTint);
            g.fillRect (area);
        }
        if (auto c = colourFor (note))       // DecentSampler playable-zone tint
        {
            g.setColour (c->withAlpha (kWhiteTintAlpha));
            g.fillRect (area);
        }
    }

    void drawBlackNote (int note, juce::Graphics& g, juce::Rectangle<float> area,
                        bool isDown, bool isOver, juce::Colour noteFillColour) override
    {
        juce::MidiKeyboardComponent::drawBlackNote (note, g, area, isDown, isOver, noteFillColour);
        if (blackKeyTint)                    // global aesthetic tint (honours its own alpha)
        {
            g.setColour (*blackKeyTint);
            g.fillRect (area);
        }
        if (auto c = colourFor (note))       // DecentSampler playable-zone tint
        {
            g.setColour (c->withAlpha (kBlackTintAlpha));
            g.fillRect (area);
        }
    }

private:
    struct Range { int lo, hi; juce::Colour colour; };
    juce::Array<Range> parsed;
    std::optional<juce::Colour> whiteKeyTint, blackKeyTint;

    // ARGB hex → colour honouring its own alpha. Empty/"none"/alpha-0 → no tint.
    static std::optional<juce::Colour> parseTint (const juce::String& hex)
    {
        const auto s = hex.trim();
        if (s.isEmpty() || s.equalsIgnoreCase ("none") || s.equalsIgnoreCase ("transparent"))
            return std::nullopt;
        const auto colour = juce::Colour ((juce::uint32) s.getHexValue32());
        if (colour.getAlpha() == 0)          // no alpha given → nothing to overlay
            return std::nullopt;
        return colour;
    }

    std::optional<juce::Colour> colourFor (int note) const
    {
        for (const auto& r : parsed)
            if (note >= r.lo && note <= r.hi)
                return r.colour;
        return std::nullopt;
    }
};

} // namespace dm
