#pragma once

// Reusable plugin editor (shared across products). Renders the active mode's
// data-driven face (ManifestUiComponent) + a mode selector, bend-range stepper,
// pitch/mod wheels, and a coloured on-screen keyboard restricted to the in-use
// range. Drives/reflects the APVTS params (dm::params); the host applies them to
// the engine. Plugin-specific bits (image loading, the processor's accessors) come
// through ManifestEditorHost, so each plugin's AudioProcessorEditor is a thin host.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <model/Manifest.h>
#include "ManifestUiComponent.h"
#include "ColouredKeyboard.h"
#include "WheelLookAndFeel.h"
#include "StandaloneWindowLookAndFeel.h"
#include "LevelMeter.h"
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dm
{

/** What the reusable editor needs from the plugin/processor. */
class ManifestEditorHost
{
public:
    virtual ~ManifestEditorHost() = default;

    virtual juce::AudioProcessorValueTreeState& getApvts() = 0;
    virtual juce::MidiKeyboardState&            getKeyboardState() = 0;
    virtual juce::StringArray getModeNames() const = 0;
    virtual int               getActiveModeIndex() const = 0;
    virtual const Mode*       getActiveMode() const = 0;     // its .ui is the face to render
    virtual void setPitchWheel (int value14) = 0;            // 0..16383
    virtual void setModWheel   (int value7)  = 0;            // CC1, 0..127
    virtual juce::Image loadImage (const juce::String& id) = 0;   // resolve a manifest image id

    /** Output peak (max |sample|) since the previous call, for the level meter; the
        implementation reads-and-resets. Default 0 = no meter (host opts in). */
    virtual float readOutputPeak() { return 0.0f; }

    /** Per-channel output peaks since the previous call, for the stereo meter. Default:
        mono — both channels get readOutputPeak(). */
    virtual void readOutputPeaks (float& outL, float& outR) { outL = outR = readOutputPeak(); }

    /** Plugin version string (e.g. "0.1.0") shown in the bottom strip. Empty → no version. */
    virtual juce::String getPluginVersion() const { return {}; }

    /** Plugin display name (e.g. "Strykebrett") shown in the bottom strip. Empty → none. */
    virtual juce::String getPluginName() const { return {}; }

    /** Async load state for the progress overlay. isLoading()=true while the active mode
        is decoding on the background thread; loadProgress() is 0..1. */
    virtual bool  isLoading() const { return false; }
    virtual float loadProgress() const { return 1.0f; }

    /** Called on the message thread each editor tick — lets the host do periodic
        message-thread housekeeping (e.g. free retired modes). */
    virtual void pollEngine() {}

    /** A UI button was clicked (its index in the tab). Lets the host resolve radio-style
        button groups by "last clicked wins" — momentary buttons that all target the same
        effects (e.g. Strykebrett's ensemble O/Acc/Solo/Organ). Default: no-op. */
    virtual void noteButtonClicked (int /*buttonIndex*/) {}

    /** The editor installs this; the host invokes it (message thread) after the
        active mode actually changes, so the editor can rebuild its face. */
    std::function<void()> onModeChanged;
};

class ManifestEditor : public juce::Component,
                       private juce::Timer
{
public:
    explicit ManifestEditor (ManifestEditorHost& host);
    ~ManifestEditor() override;

    /** Natural size for this library (manifest ui dimensions + the top strip). */
    int preferredWidth() const;
    int preferredHeight() const;

    bool handleKey (const juce::KeyPress& key);   // Z / X octave shift (host wrapper forwards too)

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;   // theme the Standalone window when attached
    bool keyPressed (const juce::KeyPress& key) override { return handleKey (key); }

private:
    void rebuildUi();
    void applyPreferredSize();   // resize host to the active mode (per-mode cropTop → varying height)
    void refreshWidgets();
    void setParam (const char* paramId, float nativeValue);
    void shiftKeyboardOctave (int deltaOctaves);
    void timerCallback() override;

    ManifestEditorHost& host;

    juce::Label    versionLabel, modeLabel, bendLabel, creditLabel;   // versionLabel + creditLabel: bottom strip
    juce::ComboBox modeSelector;
    ColouredKeyboard keyboard;
    juce::Slider pitchWheel, modWheel;                 // left of the keyboard
    juce::Slider pitchDriftWheel, volDriftWheel;       // right of the keyboard
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchDriftAttachment, volDriftAttachment;
    juce::Label pitchWheelLabel, modWheelLabel, pitchDriftLabel, volDriftLabel;   // captions under each wheel
    juce::Label leftGroupLabel, rightGroupLabel;   // headers over each wheel pair ("Controls" / "Drift")

    /** Translucent backdrop behind the wheels/labels/keyboard row. A real component
        (layered above the manifest face but below the wheels) so it shows over a plugin's
        opaque background image, giving the white labels contrast on any theme. */
    struct BackdropPanel : juce::Component
    {
        void paint (juce::Graphics& g) override
        {
            g.setColour (juce::Colour (0xff000102).withAlpha (0.5f));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.0f);
        }
    };
    BackdropPanel bottomPanel;

    /** Full-cover overlay shown while the active mode decodes on the background thread.
        The window is already at its manifest size, so this bar is actually visible. */
    struct LoadingOverlay : juce::Component
    {
        double progress { 0.0 };
        juce::ProgressBar bar { progress };
        LoadingOverlay()
        {
            bar.setColour (juce::ProgressBar::backgroundColourId, juce::Colour (0xff2c2d2e));
            bar.setColour (juce::ProgressBar::foregroundColourId, juce::Colour (0xff6a9bd1));
            addAndMakeVisible (bar);
            setInterceptsMouseClicks (true, true);   // swallow clicks on the loading face
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff141516).withAlpha (0.92f));
            g.setColour (juce::Colour (0xffe8e9ea));
            g.setFont (juce::Font (juce::FontOptions (16.0f)));
            g.drawText ("Loading samples...", getLocalBounds().withTrimmedBottom (40),
                        juce::Justification::centred);
        }
        void resized() override { bar.setBounds (getLocalBounds().withSizeKeepingCentre (300, 20)); }
    };
    LoadingOverlay loadingOverlay;
    WheelLookAndFeel wheelLnf;
    StandaloneWindowLookAndFeel standaloneLnf;
    juce::LookAndFeel_V4 stripLnf;   // grayscale scheme for the top-strip combo/steppers
    juce::Component::SafePointer<juce::DocumentWindow> themedWindow;   // standalone window we styled (if any)
    juce::Slider bendRangeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bendRangeAttachment;
    juce::ToggleButton skipMutedButton { "Poly-save" };   // skip triggering muted groups (drawbars fully down)
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> skipMutedAttachment;
    juce::Slider masterSlider;   // master output fader (top strip, between "Out" and the meter)
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttachment;
    LevelMeter outputMeter;
    juce::Label meterLabel;

    int keyOctave { 6 };
    int lowestVisibleKey { 36 };
    int usedLow { 36 }, usedHigh { 96 };
    bool showModeSelector { true };   // hidden for single-mode plugins (nothing to pick)
    std::unique_ptr<ManifestUiComponent> uiComponent;

    // 30 Hz refresh caches — APVTS param POINTERS, resolved lazily on first poll and
    // cleared in rebuildUi (a mode switch changes the control set). Building the string
    // param ids (controlKey / buttonParamId) per widget per tick allocated thousands of
    // strings a second just to read values.
    std::unordered_map<const Control*, std::atomic<float>*> knobParamCache;
    std::vector<std::optional<std::atomic<float>*>> buttonParamCache;
    std::atomic<float>* chordOrderParam { nullptr };
    bool chordOrderResolved { false };

    juce::TooltipWindow tooltipWindow { this };   // shows control tooltips (e.g. Poly-save)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestEditor)
};

/** Thin AudioProcessorEditor that simply hosts a ManifestEditor. A product's
    createEditor can return one of these directly. */
class ManifestPluginEditor : public juce::AudioProcessorEditor
{
public:
    ManifestPluginEditor (juce::AudioProcessor& proc, ManifestEditorHost& host)
        : juce::AudioProcessorEditor (proc), editor (host)
    {
        addAndMakeVisible (editor);
        setSize (editor.preferredWidth(), editor.preferredHeight());
    }

    void resized() override { editor.setBounds (getLocalBounds()); }
    bool keyPressed (const juce::KeyPress& key) override { return editor.handleKey (key); }

private:
    ManifestEditor editor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ManifestPluginEditor)
};

} // namespace dm
