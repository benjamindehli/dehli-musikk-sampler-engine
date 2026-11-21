// Names a screen reader gives the editor's controls. The knobs and buttons are
// drawn from the manifest, and the editor adds its own wheels, master fader
// and mode menu. Every one needs a name, and no two may share one: a "Reverb"
// knob beside a "Reverb" button leaves a listener guessing which is which.

#include <ManifestPluginProcessor.h>

#include <cstring>
#include <functional>
#include <map>
#include <memory>

namespace {

const char *kManifest = R"({
    "schema": 1,
    "modes": [
      { "name": "One",
        "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
        "groups": [],
        "ui": { "width":600, "height":300, "tabs": [ { "name":"main",
          "controls": [
            { "id":"k1", "label":"envelope_attack", "rect":{"x":10,"y":10,"width":40,"height":40},
              "min":0, "max":1, "value":0, "skin":{"image":"knob","numFrames":1} },
            { "id":"k2", "label":"Reverb", "rect":{"x":60,"y":10,"width":40,"height":40},
              "min":0, "max":1, "value":0, "skin":{"image":"knob","numFrames":1} } ],
          "buttons": [
            { "id":"b1", "rect":{"x":110,"y":10,"width":30,"height":20},
              "states": [ {"name":"reverb_off"}, {"name":"reverb_on"} ] },
            { "id":"b2", "rect":{"x":150,"y":10,"width":30,"height":20},
              "states": [ {"name":"Off"}, {"name":"On"} ] },
            { "id":"b3", "rect":{"x":190,"y":10,"width":30,"height":20},
              "states": [ {"name":"keyboard_mono"}, {"name":"keyboard_poly"} ] },
            { "id":"b4", "label":"vibrato_depth", "rect":{"x":110,"y":40,"width":30,"height":20},
              "states": [ {"name":"Light"}, {"name":"Deep"} ] } ],
          "menus": [
            { "id":"m1", "rect":{"x":230,"y":10,"width":80,"height":20},
              "options": [ {"name":"Short"}, {"name":"Long"} ] },
            { "id":"m2", "label":"Chord order", "rect":{"x":230,"y":40,"width":80,"height":20},
              "options": [ {"name":"Up"}, {"name":"Down"} ] } ] } ] } },
      { "name": "Two",
        "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
        "groups": [] } ]
})";

class AccessibilityTests : public juce::UnitTest {
public:
  AccessibilityTests() : juce::UnitTest("Accessibility", "engine") {}

  void runTest() override {
    beginTest("every control has a name, and no two share one");

    dm::ManifestPluginProcessor::Assets a;
    a.name = "Test Instrument";
    a.version = "0.0.0";
    a.manifestJson = kManifest;
    a.manifestJsonSize = (int)std::strlen(kManifest);
    a.findResource = [](const juce::String &, int &size) -> const char * {
      size = 0;
      return nullptr;
    };
    auto p = std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
    expect(editor != nullptr, "the editor opens");
    if (editor == nullptr)
      return;

    std::map<juce::String, int> names;
    int controls = 0, nameless = 0;
    std::function<void(juce::Component &)> walk = [&](juce::Component &c) {
      for (auto *child : c.getChildren()) {
        const juce::String type = typeid(*child).name();
        const bool control = dynamic_cast<juce::Slider *>(child) != nullptr ||
                             dynamic_cast<juce::Button *>(child) != nullptr ||
                             dynamic_cast<juce::ComboBox *>(child) != nullptr ||
                             type.contains("FilmstripKnob") ||
                             type.contains("ImageStateButton");
        if (control && child->isVisible()) {
          ++controls;
          auto title = child->getTitle();
          if (title.isEmpty())
            if (auto *b = dynamic_cast<juce::Button *>(child))
              title = b->getButtonText();
          if (title.isEmpty())
            ++nameless;
          else
            ++names[title];
        }
        walk(*child);
      }
    };
    walk(*editor);

    expect(controls >= 10, "the walk reached the manifest and editor controls");
    expectEquals(nameless, 0, "controls without a name");
    for (const auto &[title, count] : names)
      expectEquals(count, 1, "controls named " + title);

    // The shape of each kind of name.
    expect(names.count("Envelope attack") == 1,
           "a knob is named after its label, underscores read as spaces");
    expect(names.count("Reverb") == 1 && names.count("Reverb (Off / On)") == 1,
           "a button whose name a knob has adds its states");
    expect(names.count("Keyboard") == 1,
           "a button takes the word its states share");
    expect(names.count("Button 2") == 1,
           "a button whose states share nothing takes its host name");
    expect(names.count("Menu 1") == 1, "a menu is named");
    expect(names.count("Vibrato depth") == 1,
           "a button's label names it, whatever its states share");
    expect(names.count("Chord order") == 1, "and a menu's label names it");

    // The host shows the same words for the button's parameter, and the id
    // stays the one the button's id gives it.
    bool hostName = false;
    for (auto *param : p->getParameters())
      if (auto *withId =
              dynamic_cast<juce::AudioProcessorParameterWithID *>(param))
        if (withId->getName(64) == "Vibrato depth")
          hostName = !withId->paramID.containsIgnoreCase("vibrato");
    expect(hostName, "the host names the button's parameter by its label too");
    expect(names.count("Pitch bend") == 1 && names.count("Output level") == 1 &&
               names.count("Mode") == 1,
           "the editor names its own controls");
  }
};

AccessibilityTests accessibilityTests;
} // namespace
