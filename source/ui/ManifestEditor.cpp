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

    const auto modeNames = host.getModeNames();
    showModeSelector = modeNames.size() > 1;   // a single-mode plugin has nothing to select

    modeLabel.setText ("Mode", juce::dontSendNotification);
    modeLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
    modeLabel.setVisible (showModeSelector);
    addChildComponent (modeLabel);

    for (int i = 0; i < modeNames.size(); ++i)
        modeSelector.addItem (modeNames[i], i + 1);
    if (! modeNames.isEmpty())
        modeSelector.setSelectedId (host.getActiveModeIndex() + 1, juce::dontSendNotification);
    modeSelector.setTextWhenNoChoicesAvailable ("(no embedded assets)");
    modeSelector.onChange = [this]
    {
        setParam (params::id::mode, (float) (modeSelector.getSelectedId() - 1));
    };
    modeSelector.setVisible (showModeSelector);
    addChildComponent (modeSelector);

    bottomPanel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (bottomPanel);

    loadingOverlay.setVisible (false);
    addChildComponent (loadingOverlay);   // shown by the timer while a mode decodes

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
        c.first->setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
        c.first->setFont (juce::Font (juce::FontOptions().withHeight (10.0f)));
        c.first->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*c.first);
    }

    bendLabel.setText ("Bend", juce::dontSendNotification);
    bendLabel.setJustificationType (juce::Justification::centredRight);
    bendLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
    addAndMakeVisible (bendLabel);

    bendRangeSlider.setSliderStyle (juce::Slider::IncDecButtons);
    bendRangeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 22);
    bendRangeSlider.setTextValueSuffix (" st");
    addAndMakeVisible (bendRangeSlider);
    bendRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::pitchBendRange, bendRangeSlider);

    // Performance/fidelity toggle: when on (default), notes skip drawbars pulled fully
    // down; off = every group triggers so raising a drawbar mid-note brings it in.
    skipMutedButton.setColour (juce::ToggleButton::textColourId, juce::Colour (0xfffdfeff));
    skipMutedButton.setTooltip ("Poly-save: notes skip drawbars/groups pulled fully down, saving "
                                "polyphony and CPU. Turn off to let raising a drawbar while a note is "
                                "held bring it in (uses more voices).");
    addAndMakeVisible (skipMutedButton);
    skipMutedAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
        host.getApvts(), params::id::skipMuted, skipMutedButton);

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
    meterLabel.setColour (juce::Label::textColourId, juce::Colour (0xfffdfeff));
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

    // New mode = new control set → the refresh caches' Control* keys / button indices
    // no longer correspond. They repopulate lazily on the next timer tick.
    knobParamCache.clear();
    buttonParamCache.clear();
    chordOrderParam = nullptr;
    chordOrderResolved = false;

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
    uiComponent->onButtonChanged = [this] (const Button& btn, int buttonIndex, int stateIndex)
    {
        setParam (params::buttonParamId (btn, buttonIndex).toRawUTF8(), (float) stateIndex);
        host.noteButtonClicked (buttonIndex);   // radio groups: last clicked wins

        // Button links: e.g. turning Stereo on auto-enables Double Track. Fire the
        // dependent button's param (its widget refreshes + the host applies it).
        // Endpoints resolve by id when the link carries one (hand-authored manifests),
        // falling back to the legacy indices.
        if (const auto* m = host.getActiveMode())
        {
            if (m->ui.tabs.isEmpty())
                return;
            const auto& btns = m->ui.tabs.getReference (0).buttons;
            auto resolve = [&btns] (const juce::String& linkId, int fallback) -> int
            {
                if (linkId.isNotEmpty())
                    for (int i = 0; i < btns.size(); ++i)
                        if (btns.getReference (i).id == linkId)
                            return i;
                return fallback;
            };
            for (const auto& link : m->ui.buttonLinks)
            {
                const int from = resolve (link.fromId, link.fromButton);
                const int to   = resolve (link.toId,   link.toButton);
                if (from == buttonIndex && link.fromState == stateIndex
                    && to >= 0 && to < btns.size() && link.toState >= 0)
                    setParam (params::buttonParamId (btns.getReference (to), to).toRawUTF8(),
                              (float) link.toState);
            }
        }
    };
    uiComponent->onMenuChanged = [this] (const Menu& menu, int idx)
    {
        // Only the FIRST dropdown maps to the chordOrder param (that's the menu the
        // param represents — see createLayout); a second menu must not clobber it.
        // NOTE: compare structurally, not by address — ManifestUiComponent holds a
        // COPY of the Ui tree, so `&menu` never equals the mode's own menu object.
        if (isFirstMenu (menu))
            setParam (params::id::chordOrder, (float) idx);
    };
    addAndMakeVisible (*uiComponent);
    // z-order (back → front): manifest face, then the translucent backdrop, then the
    // wheels/labels/keyboard sit on top. Push the panel back first so the face lands
    // behind it after uiComponent->toBack().
    bottomPanel.toBack();
    uiComponent->toBack();

    keyboard.setColourRanges (ui.keyboardColors);
    keyboard.setKeyTints (ui.whiteKeyTint, ui.blackKeyTint);

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

bool ManifestEditor::isFirstMenu (const Menu& menu) const
{
    // Structural match against the active mode's first menu — the ManifestUiComponent
    // works on a COPY of the Ui tree, so pointer identity is meaningless here. With
    // zero/one menu (every current library) the answer is trivially yes.
    const auto* m = host.getActiveMode();
    if (m == nullptr || m->ui.tabs.isEmpty())
        return true;
    const auto& menus = m->ui.tabs.getReference (0).menus;
    if (menus.size() < 2)
        return true;
    const auto& first = menus.getReference (0);
    if (menu.id.isNotEmpty() && first.id.isNotEmpty())
        return menu.id == first.id;
    if (menu.controlIndex && first.controlIndex)
        return *menu.controlIndex == *first.controlIndex;
    return true;   // indistinguishable — preserve the pre-guard behaviour
}

void ManifestEditor::refreshWidgets()
{
    if (uiComponent == nullptr)
        return;

    // Runs 30×/s for every widget — so the param LOOKUP (string id build + map search)
    // happens once per widget per rebuilt UI, cached as a raw atomic pointer; the tick
    // itself is just atomic loads. Caches are cleared in rebuildUi.
    auto& apvts = host.getApvts();

    uiComponent->refresh (
        [&] (const Control& c) -> std::optional<double>
        {
            auto it = knobParamCache.find (&c);
            if (it == knobParamCache.end())
            {
                std::atomic<float>* a = nullptr;
                const auto ids = params::controlParamIds (c);
                if (! ids.isEmpty())
                    a = apvts.getRawParameterValue (ids[0]);
                it = knobParamCache.emplace (&c, a).first;
            }
            if (it->second == nullptr) return std::nullopt;
            const double mn = c.min.value_or (0.0), mx = c.max.value_or (1.0);
            return mn + (double) it->second->load() * (mx - mn);
        },
        [&] (const Button& b, int index) -> std::optional<int>
        {
            if ((int) buttonParamCache.size() <= index)
                buttonParamCache.resize ((size_t) index + 1);
            auto& slot = buttonParamCache[(size_t) index];
            if (! slot.has_value())
            {
                const auto pid = params::buttonParamId (b, index);
                slot = apvts.getParameter (pid) != nullptr ? apvts.getRawParameterValue (pid)
                                                           : nullptr;
            }
            if (*slot == nullptr) return std::nullopt;
            return (int) (*slot)->load();
        },
        [&] (const Menu& m) -> std::optional<int>
        {
            // Only the FIRST dropdown reflects chordOrder (see onMenuChanged).
            if (! isFirstMenu (m))
                return std::nullopt;
            if (! chordOrderResolved)
            {
                chordOrderParam = apvts.getRawParameterValue (params::id::chordOrder);
                chordOrderResolved = true;
            }
            return (int) (chordOrderParam != nullptr ? chordOrderParam->load() : 0.0f);
        });
}

void ManifestEditor::timerCallback()
{
    refreshWidgets();
    float peakL = 0.0f, peakR = 0.0f;
    host.readOutputPeaks (peakL, peakR);
    outputMeter.setLevels (peakL, peakR);

    host.pollEngine();   // message-thread housekeeping (free retired modes)

    // Show the loading overlay while the active mode decodes on the background thread.
    const bool loading = host.isLoading();
    if (loading)
    {
        loadingOverlay.progress = juce::jlimit (0.0, 1.0, (double) host.loadProgress());
        applyPreferredSize();   // keep the window at full size during load so the bar is visible
                                // (the standalone's startup layout otherwise leaves it tiny)
    }
    if (loading != loadingOverlay.isVisible())
    {
        loadingOverlay.setVisible (loading);
        if (loading)
            loadingOverlay.toFront (false);
        else
            applyPreferredSize();   // load finished — re-assert the full window size
    }
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
    // Min 0 (not 1) so the QWERTY play-octave can reach MIDI note 0 (C-2) — SubC's range
    // starts there. Matches the load-time init clamp; min 1 stranded octave 0 after any shift.
    keyOctave = juce::jlimit (0, 9, keyOctave + deltaOctaves);
    keyboard.setKeyPressBaseOctave (keyOctave);

    // Scroll the visible window ONLY when the range is genuinely wider than the keyboard.
    // When the whole range fits, scrolling would expose blank keyboard past the last key
    // (a shadowless white "box" beyond E5). When it doesn't fit, clamp the first visible key
    // so the highest key stays flush against the right edge rather than leaving that same gap.
    const float totalW = keyboard.getTotalKeyboardWidth();
    const float viewW  = (float) keyboard.getWidth();
    if (totalW <= viewW + 1.0f)
    {
        lowestVisibleKey = usedLow;
    }
    else
    {
        const float maxScroll = totalW - viewW;
        int maxFirst = usedLow;
        for (int n = usedLow; n <= usedHigh; ++n)
        {
            if (keyboard.getKeyStartPosition (n) <= maxScroll) maxFirst = n;
            else break;
        }
        lowestVisibleKey = juce::jlimit (usedLow, maxFirst, lowestVisibleKey + deltaOctaves * 12);
    }
    keyboard.setLowestVisibleKey (lowestVisibleKey);
}

void ManifestEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff191a1b));   // top + bottom strip background
}

void ManifestEditor::resized()
{
    auto area = getLocalBounds();

    auto top = area.removeFromTop (kTopStrip);
    // Combo + steppers are trimmed to the text-box height (22) and centred in the
    // strip so their heights match the number inputs.
    constexpr int kCtrlH = 22;
    if (showModeSelector)   // single-mode plugins hide the selector; the rest shift left
    {
        modeLabel.setBounds (top.removeFromLeft (46));
        modeSelector.setBounds (top.removeFromLeft (220).withSizeKeepingCentre (220, kCtrlH));
        top.removeFromLeft (12);
    }
    bendLabel.setBounds (top.removeFromLeft (40));
    bendRangeSlider.setBounds (top.removeFromLeft (96).withSizeKeepingCentre (96, kCtrlH));
    top.removeFromLeft (16);
    skipMutedButton.setBounds (top.removeFromLeft (110).withSizeKeepingCentre (110, kCtrlH));

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

    const int kbHeight = 90, vMargin = 10;
    const int edgeMargin = 6;   // translucent panel ↔ window left/right edges
    const int gap = 8;          // panel ↔ wheels, wheel ↔ wheel, wheels ↔ keyboard
    const int wheelW = 20, labelH = 13;
    const int wheelTop = area.getBottom() - vMargin - kbHeight;
    const int wheelH   = kbHeight - 2 * labelH;   // header above + caption below → wheel vertically centred
    const int wheelY   = wheelTop + labelH;

    auto placeWheel = [&] (juce::Slider& w, juce::Label& lbl, int x)
    {
        w.setBounds (x, wheelY, wheelW, wheelH);
        lbl.setBounds (x - 8, wheelY + wheelH, wheelW + 16, labelH);   // caption centred under the wheel
    };

    const int panelLeft  = area.getX() + edgeMargin;
    const int panelRight = area.getRight() - edgeMargin;

    // Two wheels each side, `gap` in from the panel → the keyboard stays centred.
    const int lx = panelLeft + gap;
    placeWheel (pitchWheel, pitchWheelLabel, lx);
    placeWheel (modWheel,   modWheelLabel,   lx + wheelW + gap);

    const int rx2 = panelRight - gap - wheelW;   // outermost right wheel
    const int rx1 = rx2 - wheelW - gap;          // inner right wheel
    placeWheel (pitchDriftWheel, pitchDriftLabel, rx1);
    placeWheel (volDriftWheel,   volDriftLabel,   rx2);

    // A header centred over each wheel pair.
    const int pairW = 2 * wheelW + gap;
    leftGroupLabel.setBounds  (lx,  wheelTop, pairW, labelH);
    rightGroupLabel.setBounds (rx1, wheelTop, pairW, labelH);

    const int kbLeft  = modWheel.getRight() + gap;
    const int kbRight = pitchDriftWheel.getX() - gap;
    const int availW  = juce::jmax (0, kbRight - kbLeft);
    keyboard.setBounds (kbLeft, wheelTop, availW, kbHeight);   // provisional; may shrink to fit below

    // One translucent panel behind the whole row (wheels + labels + keyboard).
    bottomPanel.setBounds (juce::Rectangle<int>::leftTopRightBottom (
        panelLeft, wheelTop - 4, panelRight, wheelTop + kbHeight + 4));

    // Probe the component's OWN white-key count at keyWidth 1 — counting them ourselves is
    // off-by-one vs what MidiKeyboardComponent renders at the range ends. Then pick an INTEGER
    // key width: floor (integer division) so the keys never spill past the width.
    keyboard.setKeyWidth (1.0f);
    const int whiteKeys = juce::jmax (1, (int) keyboard.getTotalKeyboardWidth());
    const int keyW      = juce::jmax (12, availW / whiteKeys);
    keyboard.setKeyWidth ((float) keyW);

    // When the range fits, size the keyboard to EXACTLY its keys and centre it. Otherwise MKC
    // paints the leftover component width as blank white keys past the last note — a shadowless
    // "box" you can still scroll/drag into even at a near-exact fit (rounding leaves the content
    // a hair wider than the view). Sizing the component to the content removes that region
    // entirely; the small centred margin shows the dark panel behind, not white. A range too
    // wide to fit keeps the full width and scrolls (its own high keys, never blank space).
    const int contentW = whiteKeys * keyW;
    if (contentW < availW)
        keyboard.setBounds (kbLeft + (availW - contentW) / 2, wheelTop, contentW, kbHeight);

    loadingOverlay.setBounds (getLocalBounds());   // covers everything while decoding
}

} // namespace dm
