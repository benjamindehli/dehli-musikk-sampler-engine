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
#include "SettingsPanel.h"
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

    /** True when any mode carries sequence triggers — the settings overlay only
        shows the sequencer block for libraries that actually sequence. */
    virtual bool hasSequencer() const { return false; }

    /** True when the library declares the shared-air simulation (AirSupply) — the
        settings overlay offers its toggle. */
    virtual bool hasAirSupply() const { return false; }

    /** True in the Standalone build (no host transport to follow). The processor
        answers from its wrapperType — the only reliable source. */
    virtual bool isStandaloneBuild() const { return false; }

    /** Whether the editor may theme + resize its top-level DocumentWindow (the
        plugin's own standalone window). Hosts that embed the editor in a larger
        app window (DMSE Studio) return false. */
    virtual bool manageTopLevelWindow() const { return true; }

    /** Fresh multi-mode instance: the editor should show the mode chooser and call
        confirmModeChoice with the pick before anything decodes. */
    virtual bool needsModeChoice() const { return false; }
    virtual void confirmModeChoice (int /*index*/) {}

    // ── MIDI learn (right-click a controller) ───────────────────────────────
    // startMidiLearn arms capture: the next incoming CC (on the audio thread) maps
    // to `paramId` and learn ends. isMidiLearnActive turns false on completion or
    // cancel — the editor polls it for its banner. Mappings persist in the state.
    virtual void startMidiLearn (const juce::String& /*paramId*/) {}
    virtual void cancelMidiLearn() {}
    virtual bool isMidiLearnActive() const { return false; }
    virtual int  getMidiMappingCc (const juce::String& /*paramId*/) const { return -1; }
    virtual void removeMidiMapping (const juce::String& /*paramId*/) {}

    /** Async load state for the progress overlay. isLoading()=true while the active mode
        is decoding on the background thread; loadProgress() is 0..1. */
    virtual bool  isLoading() const { return false; }
    virtual float loadProgress() const { return 1.0f; }

    /** Called on the message thread each editor tick — lets the host do periodic
        message-thread housekeeping (e.g. free retired modes). */
    virtual void pollEngine() {}

    /** Display text for the sequencer's effective strum rate, shown in the mode's
        strumSpeedReadout zone (note value when tempo synced, steps/s when free).
        Empty hides the readout text. */
    virtual juce::String strumSpeedText() const { return {}; }

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
    void mouseDown (const juce::MouseEvent&) override;   // right-click outside the face → Settings menu
    void parentHierarchyChanged() override;   // theme the Standalone window when attached
    bool keyPressed (const juce::KeyPress& key) override { return handleKey (key); }

private:
    void rebuildUi();
    void applyPreferredSize();   // resize host to the active mode (per-mode cropTop → varying height)
    void refreshWidgets();
    bool isFirstMenu (const Menu& menu) const;   // structural (Ui tree is copied in the renderer)
    void setParam (const char* paramId, float nativeValue);
    void openSettings();
    void showContextMenu (const juce::String& paramId, juce::Component* target);   // empty id → Settings only
    void shiftKeyboardOctave (int deltaOctaves);
    void timerCallback() override;

    ManifestEditorHost& host;

    juce::Label    versionLabel, modeLabel, creditLabel;   // versionLabel + creditLabel: bottom strip
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

    /** Thin captions band directly above the keyboard: manifest `keyboard.labels`
        drawn centred over their note ranges (e.g. Omnichord chord-type sections +
        strum-key roles). Positioned to share the keyboard's x-space, so key
        rectangles map 1:1; ranges scrolled out of view are clipped. */
    struct KeyLabelStrip : juce::Component
    {
        explicit KeyLabelStrip (juce::MidiKeyboardComponent& kb) : keys (kb)
        {
            setInterceptsMouseClicks (false, false);
        }
        void setLabels (const juce::Array<KeyboardLabel>& l) { labels = l; repaint(); }
        void paint (juce::Graphics& g) override
        {
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            for (const auto& kl : labels)
            {
                const auto lo = keys.getRectangleForKey (kl.loNote);
                const auto hi = keys.getRectangleForKey (kl.hiNote);
                float x1 = juce::jmin (lo.getX(), hi.getX());
                float x2 = juce::jmax (lo.getRight(), hi.getRight());
                if (x2 <= 0.0f || x1 >= (float) getWidth())
                    continue;   // range scrolled out of view
                x1 = juce::jmax (x1, 0.0f);
                x2 = juce::jmin (x2, (float) getWidth());

                const juce::Rectangle<float> r (x1, 0.0f, x2 - x1, (float) getHeight());
                g.setColour (juce::Colour (0xfffdfeff).withAlpha (0.28f));
                g.drawVerticalLine ((int) r.getX(), 2.0f, (float) getHeight() - 2.0f);
                g.drawVerticalLine ((int) r.getRight() - 1, 2.0f, (float) getHeight() - 2.0f);
                g.setColour (juce::Colour (0xffe8e9ea));
                g.drawText (kl.text, r.reduced (2.0f, 0.0f), juce::Justification::centred, false);
            }
        }
        juce::MidiKeyboardComponent& keys;
        juce::Array<KeyboardLabel> labels;
    };
    KeyLabelStrip keyLabelStrip { keyboard };
    static constexpr int kKeyLabelStrip = 16;   // strip height when the mode has labels
    int lastStripScrollKey = -1;                // repaint the strip when the keyboard scrolls

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
            bar.setPercentageDisplay (false);   // % lives in the caption below — the
                                                // on-bar text was unreadable over the fill
            addAndMakeVisible (bar);
            setInterceptsMouseClicks (true, true);   // swallow clicks on the loading face
        }
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff141516).withAlpha (0.92f));
            g.setColour (juce::Colour (0xffe8e9ea));
            g.setFont (juce::Font (juce::FontOptions (16.0f)));
            g.drawText ("Loading samples...  " + juce::String ((int) std::round (progress * 100.0)) + "%",
                        getLocalBounds().withTrimmedBottom (40), juce::Justification::centred);
        }
        void resized() override { bar.setBounds (getLocalBounds().withSizeKeepingCentre (300, 20)); }
    };
    LoadingOverlay loadingOverlay;
    WheelLookAndFeel wheelLnf;
    StandaloneWindowLookAndFeel standaloneLnf;
    juce::LookAndFeel_V4 stripLnf;   // grayscale scheme for the top-strip combo/steppers
    juce::Component::SafePointer<juce::DocumentWindow> themedWindow;   // standalone window we styled (if any)
    juce::TextButton settingsButton { "Settings" };   // opens the settings overlay
    std::unique_ptr<SettingsPanel> settingsPanel;      // created lazily on first open
    juce::TextButton learnBanner;                      // "MIDI Learn armed" banner; click = cancel

    /** Mode chooser overlay: shown on a fresh multi-mode instance so the user picks
        which mode to load instead of eagerly decoding mode 0. The last-used mode
        (the restored Mode param) is highlighted. */
    struct ModeChooser : juce::Component
    {
        std::function<void (int)> onPick;

        void setModes (const juce::StringArray& names, int highlight)
        {
            buttons.clear();
            title.setText ("Select mode", juce::dontSendNotification);
            title.setJustificationType (juce::Justification::centred);
            title.setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
            title.setFont (juce::Font (juce::FontOptions (16.0f).withStyle ("Bold")));
            addAndMakeVisible (title);
            for (int i = 0; i < names.size(); ++i)
            {
                auto* b = buttons.add (new juce::TextButton (names[i]));
                if (i == highlight)
                    b->setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3a5a7a));
                b->onClick = [this, i] { if (onPick) onPick (i); };
                addAndMakeVisible (*b);
            }
            resized();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff000102).withAlpha (0.7f));
            g.setColour (juce::Colour (0xff202122));
            g.fillRoundedRectangle (panelRect.toFloat(), 8.0f);
            g.setColour (juce::Colour (0xff555657));
            g.drawRoundedRectangle (panelRect.toFloat(), 8.0f, 1.0f);
        }

        void resized() override
        {
            const int rowH = 34, pad = 14, w = 260;
            const int h = pad * 2 + rowH + buttons.size() * rowH;
            panelRect = getLocalBounds().withSizeKeepingCentre (w, h);
            auto r = panelRect.reduced (pad);
            title.setBounds (r.removeFromTop (rowH));
            for (auto* b : buttons)
                b->setBounds (r.removeFromTop (rowH).reduced (0, 4));
        }

        juce::Label title;
        juce::OwnedArray<juce::TextButton> buttons;
        juce::Rectangle<int> panelRect;
    };
    ModeChooser modeChooser;
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
