// Undo and Redo: what a person changes in the window, one edit at a time.
// Automation and a restored session never enter the history.

#include <ManifestPluginProcessor.h>
#include <ui/ManifestUiComponent.h>

#include <cstring>
#include <functional>
#include <memory>

namespace {

// A knob, and a menu whose second option also sets the knob, as a patch does.
const char *kManifest = R"({
    "schema": 1,
    "modes": [
      { "name": "One",
        "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
        "groups": [],
        "ui": { "width":600, "height":300, "tabs": [ { "name":"main",
          "controls": [
            { "id":"k1", "label":"Tone", "rect":{"x":10,"y":10,"width":40,"height":40},
              "min":0, "max":1, "value":0, "skin":{"image":"knob","numFrames":1},
              "bindings": [ { "type":"amp", "level":"instrument", "parameter":"AMP_VOLUME" } ] } ],
          "menus": [
            { "id":"m1", "label":"Patch", "rect":{"x":60,"y":10,"width":80,"height":20},
              "options": [ {"name":"Plain"},
                           {"name":"Bright", "bindings": [
                             { "type":"control", "level":"ui", "targetId":"k1",
                               "parameter":"VALUE", "translation":"fixed_value",
                               "translationValue":"0.75" } ] } ] } ] } ] } } ]
})";

std::unique_ptr<dm::ManifestPluginProcessor> makeProcessor() {
  dm::ManifestPluginProcessor::Assets a;
  a.name = "Undo Test";
  a.version = "0.0.0";
  a.manifestJson = kManifest;
  a.manifestJsonSize = (int)std::strlen(kManifest);
  a.findResource = [](const juce::String &, int &size) -> const char * {
    size = 0;
    return nullptr;
  };
  return std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
}

class UndoTests : public juce::UnitTest {
public:
  UndoTests() : juce::UnitTest("Undo", "engine") {}

  void runTest() override {
    auto p = makeProcessor();
    auto &state = p->getApvts();
    auto *tone = state.getParameter("ctl_tone");
    expect(tone != nullptr, "the knob has a parameter");
    if (tone == nullptr)
      return;
    const auto valueOf = [&state](const char *id) {
      auto *raw = state.getRawParameterValue(id);
      return raw != nullptr ? raw->load() : -1.0f;
    };

    beginTest("a gesture is one step, undone and redone");
    expect(!p->canUndo(), "nothing to undo at first");
    tone->beginChangeGesture();
    tone->setValueNotifyingHost(0.25f);
    tone->setValueNotifyingHost(0.5f);
    tone->endChangeGesture();
    expect(p->canUndo(), "the edit can be undone");
    p->undo();
    expectWithinAbsoluteError(valueOf("ctl_tone"), 0.0f, 1.0e-4f);
    expect(p->canRedo(), "and redone");
    p->redo();
    expectWithinAbsoluteError(valueOf("ctl_tone"), 0.5f, 1.0e-4f);

    beginTest("automation is not an edit");
    p->undo();
    tone->setValueNotifyingHost(0.9f); // no gesture, as from the host
    p->undo();
    expectWithinAbsoluteError(valueOf("ctl_tone"), 0.9f, 1.0e-4f);

    beginTest("a restored session starts a new history");
    tone->beginChangeGesture();
    tone->setValueNotifyingHost(0.1f);
    tone->endChangeGesture();
    juce::MemoryBlock saved;
    p->getStateInformation(saved);
    p->setStateInformation(saved.getData(), (int)saved.getSize());
    expect(!p->canUndo(), "the history is gone");

    beginTest("a patch and the values it sets are one step");
    std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());
    dm::ManifestUiComponent *face = nullptr;
    std::function<void(juce::Component &)> find = [&](juce::Component &c) {
      for (auto *child : c.getChildren()) {
        if (auto *f = dynamic_cast<dm::ManifestUiComponent *>(child))
          face = f;
        find(*child);
      }
    };
    find(*editor);
    expect(face != nullptr, "the editor holds the face");
    if (face == nullptr)
      return;
    face->setValueById("m1", 1);
    expectWithinAbsoluteError(valueOf("ctl_tone"), 0.75f, 1.0e-4f);
    expectWithinAbsoluteError(valueOf("chordOrder"), 1.0f, 1.0e-4f);
    p->undo();
    expectWithinAbsoluteError(valueOf("ctl_tone"), 0.1f, 1.0e-4f);
    expectWithinAbsoluteError(valueOf("chordOrder"), 0.0f, 1.0e-4f);
    expect(!p->canUndo(), "one step took both back");
  }
};

UndoTests undoTests;
} // namespace
