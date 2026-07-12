#pragma once

// Settings overlay (gear button in the editor's top strip). Hosts the player-level
// options that used to crowd the strip (pitch bend range, poly-save) plus the
// settings-menu additions: separate bend up/down, max polyphony, master tuning,
// velocity curve, and the sequencer tempo-sync block (only shown for libraries
// that actually sequence). Every control is an APVTS attachment, so settings
// persist with the DAW session / standalone settings file like any parameter.

#include <juce_audio_processors/juce_audio_processors.h>
#include <params/ManifestParameters.h>

namespace dm
{

class SettingsPanel : public juce::Component
{
public:
    SettingsPanel (juce::AudioProcessorValueTreeState& state, bool showSequencer, bool isStandalone,
                   bool showAirSupply = false)
        : apvts (state), hasSequencer (showSequencer), hasAirSupply (showAirSupply),
          standalone (isStandalone)
    {
        // Dismiss by clicking the dimmed area; swallow everything else.
        setInterceptsMouseClicks (true, true);

        auto initLabel = [this] (juce::Label& l, const juce::String& text)
        {
            l.setText (text, juce::dontSendNotification);
            l.setColour (juce::Label::textColourId, juce::Colour (0xffe8e9ea));
            l.setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (l);
        };
        auto initIncDec = [this] (juce::Slider& s, const juce::String& suffix)
        {
            s.setSliderStyle (juce::Slider::IncDecButtons);
            s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
            s.setTextValueSuffix (suffix);
            addAndMakeVisible (s);
        };

        titleLabel.setText ("Settings", juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
        titleLabel.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
        addAndMakeVisible (titleLabel);

        closeButton.setButtonText ("X");
        closeButton.onClick = [this] { if (onClose) onClose(); };
        addAndMakeVisible (closeButton);

        initLabel (bendUpLabel, "Pitch bend up");
        initIncDec (bendUpSlider, " st");
        bendUpAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            apvts, params::id::pitchBendUp, bendUpSlider);

        initLabel (bendDownLabel, "Pitch bend down");
        initIncDec (bendDownSlider, " st");
        bendDownAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            apvts, params::id::pitchBendDown, bendDownSlider);

        initLabel (polyLabel, "Max polyphony");
        for (int i = 0; i < params::kNumPolyphonyChoices; ++i)
            polyBox.addItem (juce::String (params::kPolyphonyChoices[i]) + " voices", i + 1);
        addAndMakeVisible (polyBox);
        polyAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            apvts, params::id::maxPolyphony, polyBox);

        initLabel (polySaveLabel, "Poly-save");
        polySaveToggle.setTooltip ("Notes skip drawbars/groups pulled fully down, saving polyphony "
                                   "and CPU. Turn off to let raising a drawbar while a note is held "
                                   "bring it in (uses more voices).");
        addAndMakeVisible (polySaveToggle);
        polySaveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            apvts, params::id::skipMuted, polySaveToggle);

        initLabel (tuneLabel, "Master tuning");
        tuneSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        tuneSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 22);
        tuneSlider.setTextValueSuffix (" ct");
        tuneSlider.setDoubleClickReturnValue (true, 0.0);
        addAndMakeVisible (tuneSlider);
        tuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            apvts, params::id::masterTune, tuneSlider);

        initLabel (velCurveLabel, "Velocity curve");
        velCurveBox.addItemList ({ "Soft", "Linear", "Hard" }, 1);
        addAndMakeVisible (velCurveBox);
        velCurveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            apvts, params::id::velocityCurve, velCurveBox);

        if (hasAirSupply)
        {
            initLabel (airLabel, "Air supply");
            airToggle.setTooltip ("Simulates the shared blower: the more notes you hold, the "
                                  "less air each reed gets - softer, darker and slower-speaking notes.");
            addAndMakeVisible (airToggle);
            airAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                apvts, params::id::airSupply, airToggle);
        }

        if (hasSequencer)
        {
            initLabel (seqHeader, "Sequencer");
            seqHeader.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));

            initLabel (syncLabel, "Tempo sync");
            syncToggle.onClick = [this] { updateEnablement(); };
            addAndMakeVisible (syncToggle);
            syncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                apvts, params::id::seqTempoSync, syncToggle);

            initLabel (syncDawLabel, "Sync to DAW");
            if (standalone)
            {
                // No host transport to follow: shown unchecked and disabled. No
                // attachment — the parameter keeps its value for DAW sessions.
                syncDawToggle.setToggleState (false, juce::dontSendNotification);
            }
            else
            {
                syncDawToggle.onClick = [this] { updateEnablement(); };
                syncDawAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                    apvts, params::id::seqSyncDaw, syncDawToggle);
            }
            addAndMakeVisible (syncDawToggle);

            initLabel (bpmLabel, "Tempo");
            initIncDec (bpmSlider, " BPM");
            bpmAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                apvts, params::id::seqBpm, bpmSlider);

            initLabel (noteValueLabel, "Note value");
            for (int i = 0; i < params::kNumNoteValues; ++i)
                noteValueBox.addItem (params::kNoteValueLabels[i], i + 1);
            addAndMakeVisible (noteValueBox);
            noteValueAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                apvts, params::id::seqNoteValue, noteValueBox);

            updateEnablement();
        }
    }

    std::function<void()> onClose;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff000102).withAlpha (0.55f));   // dim the editor
        g.setColour (juce::Colour (0xff202122));
        g.fillRoundedRectangle (panelRect.toFloat(), 8.0f);
        g.setColour (juce::Colour (0xff555657));
        g.drawRoundedRectangle (panelRect.toFloat(), 8.0f, 1.0f);
    }

    void resized() override
    {
        const int rowH = 30, labelW = 130, pad = 14;
        const int rows = 6 + (hasAirSupply ? 1 : 0)
                       + (hasSequencer ? 5 : 0);   // + title row; seq adds header + 4
        const int panelW = 330, panelH = pad * 2 + rowH * (rows + 1);
        panelRect = getLocalBounds().withSizeKeepingCentre (panelW, panelH);

        auto r = panelRect.reduced (pad);
        auto title = r.removeFromTop (rowH);
        closeButton.setBounds (title.removeFromRight (24).withSizeKeepingCentre (22, 22));
        titleLabel.setBounds (title);

        auto row = [&] (juce::Label& l, juce::Component& c)
        {
            auto rr = r.removeFromTop (rowH);
            l.setBounds (rr.removeFromLeft (labelW));
            c.setBounds (rr.withSizeKeepingCentre (rr.getWidth(), 24));
        };
        row (bendUpLabel,   bendUpSlider);
        row (bendDownLabel, bendDownSlider);
        row (polyLabel,     polyBox);
        row (polySaveLabel, polySaveToggle);
        row (tuneLabel,     tuneSlider);
        row (velCurveLabel, velCurveBox);
        if (hasAirSupply)
            row (airLabel, airToggle);
        if (hasSequencer)
        {
            seqHeader.setBounds (r.removeFromTop (rowH));
            row (syncLabel,      syncToggle);
            row (syncDawLabel,   syncDawToggle);
            row (bpmLabel,       bpmSlider);
            row (noteValueLabel, noteValueBox);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! panelRect.contains (e.getPosition()) && onClose)
            onClose();
    }

private:
    void updateEnablement()
    {
        const bool sync       = syncToggle.getToggleState();
        const bool followHost = ! standalone && syncDawToggle.getToggleState();

        // setEnabled alone barely changes some widgets' looks — dim rows explicitly.
        auto enableRow = [] (juce::Component& control, juce::Label& label, bool on)
        {
            control.setEnabled (on);
            control.setAlpha (on ? 1.0f : 0.4f);
            label.setAlpha (on ? 1.0f : 0.4f);
        };
        enableRow (syncDawToggle, syncDawLabel, sync && ! standalone);
        enableRow (bpmSlider,     bpmLabel,     sync && ! followHost);
        enableRow (noteValueBox,  noteValueLabel, sync);
    }

    juce::AudioProcessorValueTreeState& apvts;
    const bool hasSequencer;
    const bool hasAirSupply;
    const bool standalone;   // from the processor's wrapperType (the reliable source)
    juce::Rectangle<int> panelRect;

    juce::Label titleLabel, bendUpLabel, bendDownLabel, polyLabel, polySaveLabel,
                tuneLabel, velCurveLabel, seqHeader, syncLabel, syncDawLabel, bpmLabel,
                noteValueLabel, airLabel;
    juce::TextButton closeButton;
    juce::Slider bendUpSlider, bendDownSlider, tuneSlider, bpmSlider;
    juce::ComboBox polyBox, velCurveBox, noteValueBox;
    juce::ToggleButton polySaveToggle, syncToggle, syncDawToggle, airToggle;

    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
        bendUpAttachment, bendDownAttachment, tuneAttachment, bpmAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        polyAttachment, velCurveAttachment, noteValueAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        polySaveAttachment, syncAttachment, syncDawAttachment, airAttachment;
};

} // namespace dm
