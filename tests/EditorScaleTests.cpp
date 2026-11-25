// The plugin's window can be dragged from the instrument's own size to twice
// it. The editor inside keeps its layout and is scaled, the window keeps the
// mode's shape, and the scale is kept with the session.

#include <ManifestPluginProcessor.h>
#include <ui/ManifestEditor.h>

#include <cstring>
#include <functional>
#include <memory>

namespace {

const char *kManifest = R"({
    "schema": 1,
    "modes": [
      { "name": "One",
        "amp": { "attack":0, "decay":0, "sustain":1, "release":0, "volume":1, "velTrack":0 },
        "groups": [],
        "ui": { "width":600, "height":300, "tabs": [ { "name":"main", "controls": [] } ] } } ]
})";

std::unique_ptr<dm::ManifestPluginProcessor> makeProcessor() {
  dm::ManifestPluginProcessor::Assets a;
  a.name = "Scale Test";
  a.version = "0.0.0";
  a.manifestJson = kManifest;
  a.manifestJsonSize = (int)std::strlen(kManifest);
  a.findResource = [](const juce::String &, int &size) -> const char * {
    size = 0;
    return nullptr;
  };
  return std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
}

class EditorScaleTests : public juce::UnitTest {
public:
  EditorScaleTests() : juce::UnitTest("Editor scale", "engine") {}

  void runTest() override {
    beginTest("the window scales its editor and keeps the scale");
    auto p = makeProcessor();
    std::unique_ptr<juce::AudioProcessorEditor> window(p->createEditor());
    expect(window != nullptr, "the editor opens");
    if (window == nullptr)
      return;

    const int w = window->getWidth(), h = window->getHeight();
    expect(window->isResizable(), "the window can be resized");
    if (auto *limits = window->getConstrainer()) {
      expectEquals(limits->getMinimumWidth(), w, "smallest is its own size");
      expectEquals(limits->getMaximumWidth(), w * 2, "largest is twice that");
      expectWithinAbsoluteError(limits->getFixedAspectRatio(),
                                (double)w / (double)h, 1.0e-9);
    } else {
      expect(false, "the window has limits");
    }

    window->setSize(w * 3 / 2, h * 3 / 2);
    juce::Component *inner = nullptr;
    for (auto *child : window->getChildren())
      if (dynamic_cast<dm::ManifestEditor *>(child) != nullptr)
        inner = child;
    expect(inner != nullptr, "the window holds the editor");
    if (inner == nullptr)
      return;
    expectEquals(inner->getWidth(), w, "the editor keeps its own layout");
    expectWithinAbsoluteError(inner->getTransform().mat00, 1.5f, 1.0e-4f);
    expectWithinAbsoluteError(
        (float)(double)p->getApvts().state.getProperty("uiScale", 1.0), 1.5f,
        1.0e-4f);

    beginTest("one resize corner, larger than JUCE's, in the bottom right");
    {
      juce::Array<juce::Component *> corners;
      for (auto *child : window->getChildren())
        if (dynamic_cast<juce::ResizableCornerComponent *>(child) != nullptr)
          corners.add(child);
      expectEquals(corners.size(), 1, "JUCE's own corner is not added too");
      if (auto *corner = corners.getFirst()) {
        const auto size = dm::ManifestPluginEditor::kCornerSize;
        expect(size > 18, "larger than JUCE's 18 pixels");
        expect(corner->isVisible() && corner->isAlwaysOnTop());
        expect(corner->getBounds() ==
                   juce::Rectangle<int>(window->getWidth() - size,
                                        window->getHeight() - size, size, size),
               corner->getBounds().toString());
      }
    }

    beginTest("Settings offers window sizes and shows the dragged one");
    {
      std::function<void(juce::Component &)> collect;
      juce::Array<juce::Component *> all;
      collect = [&](juce::Component &c) {
        for (auto *child : c.getChildren()) {
          all.add(child);
          collect(*child);
        }
      };
      collect(*window);
      for (auto *c : all)
        if (auto *b = dynamic_cast<juce::TextButton *>(c))
          if (b->getButtonText() == "Settings" && b->onClick)
            b->onClick();
      all.clear();
      collect(*window);
      juce::ComboBox *sizes = nullptr;
      for (auto *c : all)
        if (auto *box = dynamic_cast<juce::ComboBox *>(c))
          if (box->getTitle() == "Window size")
            sizes = box;
      expect(sizes != nullptr, "Settings has a Window size choice");
      if (sizes != nullptr) {
        expectEquals(sizes->getText(), juce::String("150%"),
                     "the dragged size is shown");
        sizes->setSelectedId(200, juce::sendNotificationSync);
        expectEquals(window->getWidth(), w * 2, "choosing 200% resizes");
        expectEquals(window->getHeight(), h * 2);
        window->setSize(w * 137 / 100, h * 137 / 100);
        expectEquals(sizes->getText(), juce::String("137%"),
                     "a size between the listed ones is shown as it is");
        sizes->setSelectedId(150, juce::sendNotificationSync);
        expectEquals(window->getWidth(), w * 3 / 2);
      }
    }

    beginTest("a window opened later comes back at the kept scale");
    window.reset();
    std::unique_ptr<juce::AudioProcessorEditor> again(p->createEditor());
    expectEquals(again->getWidth(), w * 3 / 2);
    expectEquals(again->getHeight(), h * 3 / 2);

    beginTest("a kept scale outside the limits is brought back inside");
    again.reset();
    p->getApvts().state.setProperty("uiScale", 9.0, nullptr);
    std::unique_ptr<juce::AudioProcessorEditor> large(p->createEditor());
    expectEquals(large->getWidth(), w * 2);
  }
};

EditorScaleTests editorScaleTests;
} // namespace
