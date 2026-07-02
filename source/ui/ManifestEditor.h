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
#include <functional>
#include <memory>

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
    void refreshWidgets();
    void setParam (const char* paramId, float nativeValue);
    void shiftKeyboardOctave (int deltaOctaves);
    void timerCallback() override;

    ManifestEditorHost& host;

    juce::Label    versionLabel, modeLabel, bendLabel;
    juce::ComboBox modeSelector;
    ColouredKeyboard keyboard;
    juce::Slider pitchWheel, modWheel;
    WheelLookAndFeel wheelLnf;
    StandaloneWindowLookAndFeel standaloneLnf;
    juce::Component::SafePointer<juce::DocumentWindow> themedWindow;   // standalone window we styled (if any)
    juce::Slider bendRangeSlider;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> bendRangeAttachment;
    LevelMeter outputMeter;
    juce::Label meterLabel;

    int keyOctave { 6 };
    int lowestVisibleKey { 36 };
    int usedLow { 36 }, usedHigh { 96 };
    std::unique_ptr<ManifestUiComponent> uiComponent;

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
