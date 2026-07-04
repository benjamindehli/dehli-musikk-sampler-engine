#include "ManifestEditor.h"
#include <params/ManifestParameters.h>
#include <SamplerEngine.h>   // version string

namespace dm
{
namespace
{
// The note range actually in use for a mode (sample zones + sequence-trigger keys +
// key-switches). Only the ENDS are stripped — the keyboard is contiguous, so unused
// keys BETWEEN used ones are kept.
juce::Range<int> usedNoteRange (const Mode& m)
{
    int lo = 128, hi = -1;
    for (const auto& g : m.groups)
        for (const auto& s : g.samples)
        {
            if (s.loNote < 0 || s.hiNote < 0)   // unmapped samples (e.g. pedal/damper noise) define no key range
                continue;
            lo = juce::jmin (lo, s.loNote); hi = juce::jmax (hi, s.hiNote);
        }
    for (const auto& t : m.sequenceTriggers) { lo = juce::jmin (lo, t.note); hi = juce::jmax (hi, t.note); }
    for (const auto& k : m.menuKeySwitches)  { lo = juce::jmin (lo, k.note); hi = juce::jmax (hi, k.note); }
    if (hi < lo) return { 36, 84 };
    return { lo, hi };
}

int countWhiteKeys (int lo, int hi)
{
    int n = 0;
    for (int note = lo; note <= hi; ++note)
    {
        const int pc = note % 12;
        if (pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11) ++n;
    }
    return juce::jmax (1, n);
}

constexpr int kTopStrip    = 28;
constexpr int kBottomStrip = 18;   // credit / plugin-name footer (smaller than the top strip)
} // namespace

ManifestEditor::ManifestEditor (ManifestEditorHost& h)
    : host (h),
      keyboard (h.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    // Bottom strip: credit on the left, "<Plugin> v<version>" on the right — small, muted.
    const auto pluginVer  = host.getPluginVersion();
    const auto pluginName = host.getPluginName();
    juce::String rightText = pluginName;
    if (pluginVer.isNotEmpty())
        rightText = (rightText.isNotEmpty() ? rightText + "  v" : "v") + pluginVer;
    versionLabel.setText (rightText, juce::dontSendNotification);
    versionLabel.setJustificationType (juce::Justification::centredRight);
    versionLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    versionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff808182));   // muted
    addAndMakeVisible (versionLabel);

    creditLabel.setText ("Benjamin Dehli  /  Dehli Musikk", juce::dontSendNotification);
    creditLabel.setJustificationType (juce::Justification::centredLeft);
    creditLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    creditLabel.setColour (juce::Label::textColourId, juce::Colour (0xff808182));   // muted
    addAndMakeVisible (creditLabel);

    modeLabel.setText ("Mode", juce::dontSendNotification);
    addAndMakeVisible (modeLabel);

    const auto modeNames = host.getModeNames();
    for (int i = 0; i < modeNames.size(); ++i)
        modeSelector.addItem (modeNames[i], i + 1);
    if (! modeNames.isEmpty())
        modeSelector.setSelectedId (host.getActiveModeIndex() + 1, juce::dontSendNotification);
    modeSelector.setTextWhenNoChoicesAvailable ("(no embedded assets)");
    modeSelector.onChange = [this]
    {
        setParam (params::id::mode, (float) (modeSelector.getSelectedId() - 1));
    };
    addAndMakeVisible (modeSelector);

    bottomPanel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (bottomPanel);

    keyboard.setAvailableRange (24, 119);
    keyboard.setLowestVisibleKey (lowestVisibleKey);
    keyboard.setKeyPressBaseOctave (keyOctave);
    addAndMakeVisible (keyboard);

    pitchWheel.setSliderStyle (juce::Slider::LinearVertical);
    pitchWheel.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    pitchWheel.setRange (0.0, 16383.0, 1.0);
    pitchWheel.setValue (8192.0, juce::dontSendNotification);
    pitchWheel.setDoubleClickReturnValue (true, 8192.0);
    pitchWheel.onValueChange = [this] { host.setPitchWheel ((int) pitchWheel.getValue()); };
    pitchWheel.onDragEnd     = [this] { pitchWheel.setValue (8192.0); };
    pitchWheel.setLookAndFeel (&wheelLnf);
    addAndMakeVisible (pitchWheel);

    modWheel.setSliderStyle (juce::Slider::LinearVertical);
    modWheel.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    modWheel.setRange (0.0, 127.0, 1.0);
    modWheel.setValue (0.0, juce::dontSendNotification);
    modWheel.onValueChange = [this] { host.setModWheel ((int) modWheel.getValue()); };
    modWheel.setLookAndFeel (&wheelLnf);
    addAndMakeVisible (modWheel);

    // Two drift wheels right of the keyboard: pitch drift + volume drift (all plugins).
    for (juce::Slider* w : { &pitchDriftWheel, &volDriftWheel })
    {
        w->setSliderStyle (juce::Slider::LinearVertical);
        w->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        w->setLookAndFeel (&wheelLnf);
        addAndMakeVisible (*w);
    }
    pitchDriftAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::pitchDrift, pitchDriftWheel);
    volDriftAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::volumeDrift, volDriftWheel);

    // A header centred over each wheel pair, and a caption under each wheel. No per-label
    // background — a single translucent panel behind the whole row (see paint()) carries
    // the contrast, so white text reads on any plugin background.
    const std::pair<juce::Label*, const char*> caps[] = {
        { &leftGroupLabel,  "Controls" }, { &rightGroupLabel, "Drift" },   // headers (over pairs)
        { &pitchWheelLabel, "Bend" }, { &modWheelLabel, "Mod" },           // left captions
        { &pitchDriftLabel, "Pitch" }, { &volDriftLabel, "Volume" } };     // right captions
    for (const auto& c : caps)
    {
        c.first->setText (c.second, juce::dontSendNotification);
        c.first->setJustificationType (juce::Justification::centred);
        c.first->setColour (juce::Label::textColourId, juce::Colour (0xffffffff));
        c.first->setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
        c.first->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*c.first);
    }

    bendLabel.setText ("Bend", juce::dontSendNotification);
    bendLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (bendLabel);

    bendRangeSlider.setSliderStyle (juce::Slider::IncDecButtons);
    bendRangeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 22);
    bendRangeSlider.setTextValueSuffix (" st");
    addAndMakeVisible (bendRangeSlider);
    bendRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::pitchBendRange, bendRangeSlider);

    masterSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);   // no inline number
    masterSlider.setTextValueSuffix (" dB");
    masterSlider.setPopupDisplayEnabled (true, false, this);   // value bubble while dragging
    addAndMakeVisible (masterSlider);
    masterAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::masterOutput, masterSlider);

    // Monochrome grayscale scheme for the top-strip combo + steppers, so they match
    // the plugin's dark theme instead of JUCE's default blue-grey.
    juce::LookAndFeel_V4::ColourScheme grayscale {
        0xff202122,  // windowBackground
        0xff2c2d2e,  // widgetBackground (combo / textbox fill)
        0xff202122,  // menuBackground (dropdown)
        0xff555657,  // outline
        0xffffffff,  // defaultText
        0xff4a4b4c,  // defaultFill (slider track/thumb, +/- button face)
        0xffffffff,  // highlightedText
        0xff6a6b6c,  // highlightedFill
        0xffffffff   // menuText
    };
    stripLnf.setColourScheme (grayscale);
    for (juce::Component* c : { static_cast<juce::Component*> (&modeSelector),
                                static_cast<juce::Component*> (&bendRangeSlider),
                                static_cast<juce::Component*> (&masterSlider) })
        c->setLookAndFeel (&stripLnf);

    meterLabel.setText ("Out", juce::dontSendNotification);
    meterLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (meterLabel);
    addAndMakeVisible (outputMeter);   // click the meter to reset its clip indicator

    host.onModeChanged = [this]
    {
        modeSelector.setSelectedId (host.getActiveModeIndex() + 1, juce::dontSendNotification);
        rebuildUi();
    };

    setWantsKeyboardFocus (true);
    rebuildUi();
    startTimerHz (30);
}

ManifestEditor::~ManifestEditor()
{
    stopTimer();
    host.onModeChanged = nullptr;
    pitchWheel.setLookAndFeel (nullptr);
    modWheel.setLookAndFeel (nullptr);
    pitchDriftWheel.setLookAndFeel (nullptr);
    volDriftWheel.setLookAndFeel (nullptr);
    modeSelector.setLookAndFeel (nullptr);
    bendRangeSlider.setLookAndFeel (nullptr);
    masterSlider.setLookAndFeel (nullptr);
    if (themedWindow != nullptr)   // detach before our LnF member dies (standalone only)
        themedWindow->setLookAndFeel (nullptr);
}

void ManifestEditor::parentHierarchyChanged()
{
    // In the Standalone build the editor's top-level parent is a DocumentWindow;
    // theme it flat monochrome. In a DAW there is no such parent → nothing to do.
    if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
    {
        if (themedWindow != dw)
        {
            themedWindow = dw;
            dw->setLookAndFeel (&standaloneLnf);
            dw->setBackgroundColour (juce::Colour (StandaloneWindowLookAndFeel::kBackground));

            // Changing the window's look-and-feel disturbs the standalone's content
            // fit; re-assert our size once the startup layout has settled.
            juce::Component::SafePointer<ManifestEditor> self { this };
            juce::MessageManager::callAsync ([self]
            {
                if (self != nullptr)
                    if (auto* w = self->themedWindow.getComponent())
                        w->setContentComponentSize (self->preferredWidth(), self->preferredHeight());
            });
        }
    }
}

int ManifestEditor::preferredWidth() const
{
    if (const auto* m = host.getActiveMode())
        if (m->ui.width > 0) return m->ui.width;
    return 812;
}

int ManifestEditor::preferredHeight() const
{
    if (const auto* m = host.getActiveMode())
        if (m->ui.height > 0)
            return juce::jmax (1, m->ui.height - m->ui.cropTop) + kTopStrip + kBottomStrip;   // cropTop trims the top
    return 375 + kTopStrip + kBottomStrip;
}

void ManifestEditor::rebuildUi()
{
    uiComponent.reset();

    const auto* mode = host.getActiveMode();
    if (mode == nullptr)
        return;
    const auto& ui = mode->ui;

    uiComponent = std::make_unique<ManifestUiComponent> (
        ui, [this] (const juce::String& id) { return host.loadImage (id); });

    uiComponent->onControlChanged = [this] (const Control& c, double native)
    {
        const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);
        const float norm = (float) (mx > mn ? juce::jlimit (0.0, 1.0, (native - mn) / (mx - mn)) : 0.0);
        for (const auto& pid : params::controlParamIds (c))
            setParam (pid.toRawUTF8(), norm);
    };
    uiComponent->onButtonChanged = [this] (const Button&, int buttonIndex, int stateIndex)
    {
        setParam (params::buttonParamId (buttonIndex).toRawUTF8(), (float) stateIndex);
        host.noteButtonClicked (buttonIndex);   // radio groups: last clicked wins
    };
    uiComponent->onMenuChanged = [this] (const Menu&, int idx)
    {
        setParam (params::id::chordOrder, (float) idx);
    };
    addAndMakeVisible (*uiComponent);
    // z-order (back → front): manifest face, then the translucent backdrop, then the
    // wheels/labels/keyboard sit on top. Push the panel back first so the face lands
    // behind it after uiComponent->toBack().
    bottomPanel.toBack();
    uiComponent->toBack();

    keyboard.setColourRanges (ui.keyboardColors);

    auto range = usedNoteRange (*mode);
    int lo = range.getStart(), hi = range.getEnd();
    // "none"/"transparent" ranges extend what's shown (regular, untinted keys the preset
    // wouldn't otherwise display). Tinted ranges don't change the extent — existing
    // libraries keep their sample-based keyboard width.
    for (const auto& kc : ui.keyboardColors)
        if (kc.color.trim().equalsIgnoreCase ("none") || kc.color.trim().equalsIgnoreCase ("transparent"))
        {
            lo = juce::jmin (lo, kc.loNote);
            hi = juce::jmax (hi, kc.hiNote);
        }
    usedLow  = juce::jlimit (0, 127, lo);
    usedHigh = juce::jlimit (0, 127, juce::jmax (hi, lo));
    keyboard.setAvailableRange (usedLow, usedHigh);
    lowestVisibleKey = usedLow;
    keyboard.setLowestVisibleKey (lowestVisibleKey);
    keyOctave = juce::jlimit (0, 9, usedLow / 12);
    keyboard.setKeyPressBaseOctave (keyOctave);

    refreshWidgets();
    resized();
    applyPreferredSize();   // modes may differ in height (per-mode cropTop) → resize host
}

void ManifestEditor::applyPreferredSize()
{
    // Resize the host to the active mode's preferred size. Modes can differ in height
    // (per-mode cropTop), so this runs on every mode switch. No-op during construction
    // (not yet parented); the plugin editor sets the initial size itself.
    const int w = preferredWidth(), h = preferredHeight();
    if (auto* parent = getParentComponent())    // the AudioProcessorEditor
        parent->setSize (w, h);
    if (themedWindow != nullptr)                // standalone window follows its content
        themedWindow->setContentComponentSize (w, h);
}

void ManifestEditor::setParam (const char* paramId, float nativeValue)
{
    if (auto* p = host.getApvts().getParameter (paramId))
        p->setValueNotifyingHost (p->convertTo0to1 (nativeValue));
}

void ManifestEditor::refreshWidgets()
{
    if (uiComponent == nullptr)
        return;

    auto& apvts = host.getApvts();
    auto raw = [&apvts] (const juce::String& pid) -> float
    {
        if (auto* a = apvts.getRawParameterValue (pid)) return a->load();
        return 0.0f;
    };

    uiComponent->refresh (
        [&] (const Control& c) -> std::optional<double>
        {
            const auto ids = params::controlParamIds (c);
            if (ids.isEmpty()) return std::nullopt;
            const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);
            return mn + (double) raw (ids[0]) * (mx - mn);
        },
        [&] (const Button&, int index) -> std::optional<int>
        {
            const auto pid = params::buttonParamId (index);
            if (host.getApvts().getParameter (pid) == nullptr) return std::nullopt;
            return (int) raw (pid);
        },
        [&] (const Menu&) -> std::optional<int>
        {
            return (int) raw (params::id::chordOrder);
        });
}

void ManifestEditor::timerCallback()
{
    refreshWidgets();
    outputMeter.setLevel (host.readOutputPeak());
}

bool ManifestEditor::handleKey (const juce::KeyPress& key)
{
    const auto c = key.getTextCharacter();
    if (c == 'z' || c == 'Z') { shiftKeyboardOctave (-1); return true; }
    if (c == 'x' || c == 'X') { shiftKeyboardOctave (+1); return true; }
    return false;
}

void ManifestEditor::shiftKeyboardOctave (int deltaOctaves)
{
    keyOctave = juce::jlimit (1, 9, keyOctave + deltaOctaves);
    keyboard.setKeyPressBaseOctave (keyOctave);

    lowestVisibleKey = juce::jlimit (usedLow, juce::jmax (usedLow, usedHigh - 12),
                                     lowestVisibleKey + deltaOctaves * 12);
    keyboard.setLowestVisibleKey (lowestVisibleKey);
}

void ManifestEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a1a));
}

void ManifestEditor::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (kTopStrip);
    // Combo + steppers are trimmed to the text-box height (22) and centred in the
    // strip so their heights match the number inputs.
    constexpr int kCtrlH = 22;
    modeLabel.setBounds (top.removeFromLeft (46));
    modeSelector.setBounds (top.removeFromLeft (220).withSizeKeepingCentre (220, kCtrlH));
    top.removeFromLeft (12);
    bendLabel.setBounds (top.removeFromLeft (40));
    bendRangeSlider.setBounds (top.removeFromLeft (96).withSizeKeepingCentre (96, kCtrlH));

    // Top-right group laid out left→right — "Out" label · master fader · level meter.
    auto outArea = top.removeFromRight (230).reduced (8, 3);
    meterLabel.setBounds (outArea.removeFromLeft (28));                 // "Out"
    outputMeter.setBounds (outArea.removeFromRight (60).reduced (0, 4)); // meter on the right
    masterSlider.setBounds (outArea.reduced (6, 0));                    // fader fills the middle

    // Bottom strip: credit (left) / plugin name + version (right).
    auto footer = area.removeFromBottom (kBottomStrip).reduced (10, 0);
    creditLabel.setBounds  (footer.removeFromLeft  (footer.getWidth() / 2));
    versionLabel.setBounds (footer);

    if (uiComponent != nullptr)
        uiComponent->setBounds (area);

    const int kbHeight = 90, margin = 10;
    const int wheelW = 22, wheelGap = 6, labelH = 13;
    const int wheelTop = area.getBottom() - margin - kbHeight;
    const int wheelH   = kbHeight - 2 * labelH;   // header above + caption below → wheel vertically centred
    const int wheelY   = wheelTop + labelH;

    auto placeWheel = [&] (juce::Slider& w, juce::Label& lbl, int x)
    {
        w.setBounds (x, wheelY, wheelW, wheelH);
        lbl.setBounds (x - 8, wheelY + wheelH, wheelW + 16, labelH);   // caption under the wheel
    };

    // Two wheels each side → the keyboard stays centred.
    const int lx = area.getX() + margin;
    placeWheel (pitchWheel, pitchWheelLabel, lx);
    placeWheel (modWheel,   modWheelLabel,   lx + wheelW + wheelGap);

    const int rx2 = area.getRight() - margin - wheelW;   // outermost right wheel
    const int rx1 = rx2 - wheelW - wheelGap;             // inner right wheel
    placeWheel (pitchDriftWheel, pitchDriftLabel, rx1);
    placeWheel (volDriftWheel,   volDriftLabel,   rx2);

    // A header centred over each wheel pair.
    const int pairW = 2 * wheelW + wheelGap;
    leftGroupLabel.setBounds  (lx,  wheelTop, pairW, labelH);
    rightGroupLabel.setBounds (rx1, wheelTop, pairW, labelH);

    const int kbLeft  = modWheel.getRight() + margin;
    const int kbRight = pitchDriftWheel.getX() - margin;
    keyboard.setBounds (kbLeft, wheelTop, juce::jmax (0, kbRight - kbLeft), kbHeight);

    // One translucent panel behind the whole row (wheels + labels + keyboard).
    bottomPanel.setBounds (juce::Rectangle<int>::leftTopRightBottom (
        lx - margin / 2, wheelTop - 4,
        rx2 + wheelW + margin / 2, wheelTop + kbHeight + 4));

    const float keyW = juce::jmax (12.0f, (float) keyboard.getWidth() / (float) countWhiteKeys (usedLow, usedHigh));
    keyboard.setKeyWidth (keyW);
}

} // namespace dm
