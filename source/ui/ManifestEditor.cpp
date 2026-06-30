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
        for (const auto& s : g.samples)   { lo = juce::jmin (lo, s.loNote); hi = juce::jmax (hi, s.hiNote); }
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

constexpr int kTopStrip = 28;
} // namespace

ManifestEditor::ManifestEditor (ManifestEditorHost& h)
    : host (h),
      keyboard (h.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    versionLabel.setText (SamplerEngine::getVersion(), juce::dontSendNotification);
    versionLabel.setJustificationType (juce::Justification::centredRight);
    versionLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (versionLabel);

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

    bendLabel.setText ("Bend", juce::dontSendNotification);
    bendLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (bendLabel);

    bendRangeSlider.setSliderStyle (juce::Slider::IncDecButtons);
    bendRangeSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 36, 22);
    bendRangeSlider.setTextValueSuffix (" st");
    addAndMakeVisible (bendRangeSlider);
    bendRangeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        host.getApvts(), params::id::pitchBendRange, bendRangeSlider);

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
        if (m->ui.height > 0) return m->ui.height + kTopStrip;
    return 375 + kTopStrip;
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
    uiComponent->onButtonChanged = [this] (const Button& b, int stateIndex)
    {
        const auto pid = params::buttonParamId (b);
        if (pid.isNotEmpty())
            setParam (pid.toRawUTF8(), stateIndex >= 1 ? 1.0f : 0.0f);
    };
    uiComponent->onMenuChanged = [this] (const Menu&, int idx)
    {
        setParam (params::id::chordOrder, (float) idx);
    };
    addAndMakeVisible (*uiComponent);
    uiComponent->toBack();

    keyboard.setColourRanges (ui.keyboardColors);

    const auto range = usedNoteRange (*mode);
    usedLow  = range.getStart();
    usedHigh = range.getEnd();
    keyboard.setAvailableRange (usedLow, usedHigh);
    lowestVisibleKey = usedLow;
    keyboard.setLowestVisibleKey (lowestVisibleKey);
    keyOctave = juce::jlimit (0, 9, usedLow / 12);
    keyboard.setKeyPressBaseOctave (keyOctave);

    refreshWidgets();
    resized();
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
        [&] (const Button& b) -> std::optional<int>
        {
            const auto pid = params::buttonParamId (b);
            if (pid.isEmpty()) return std::nullopt;
            return raw (pid) > 0.5f ? 1 : 0;
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
    modeLabel.setBounds (top.removeFromLeft (46));
    modeSelector.setBounds (top.removeFromLeft (220));
    versionLabel.setBounds (top.removeFromRight (190));
    top.removeFromLeft (12);
    bendLabel.setBounds (top.removeFromLeft (40));
    bendRangeSlider.setBounds (top.removeFromLeft (96));
    // Output meter on the right of the strip (before the version label).
    auto meterArea = top.removeFromRight (130).reduced (8, 7);
    meterLabel.setBounds (meterArea.removeFromLeft (28));
    outputMeter.setBounds (meterArea);

    if (uiComponent != nullptr)
        uiComponent->setBounds (area);

    const int kbHeight = 90, margin = 10;
    const int wheelW = 22, wheelGap = 6;
    const int wheelTop = area.getBottom() - margin - kbHeight;

    pitchWheel.setBounds (area.getX() + margin,             wheelTop, wheelW, kbHeight);
    modWheel.setBounds   (pitchWheel.getRight() + wheelGap, wheelTop, wheelW, kbHeight);

    const int sideGap = modWheel.getRight() + margin;
    keyboard.setBounds (sideGap, wheelTop, area.getRight() - 2 * sideGap, kbHeight);

    const float keyW = juce::jmax (12.0f, (float) keyboard.getWidth() / (float) countWhiteKeys (usedLow, usedHigh));
    keyboard.setKeyWidth (keyW);
}

} // namespace dm
