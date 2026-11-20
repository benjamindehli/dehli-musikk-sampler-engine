// The opt-in update check: deciding whether a release is newer, reading the
// feed a product page publishes, keeping the player's answer, and the card and
// link the editor shows. The fetch needs a network and is not run here, so no
// test lets the check start.

#include <ManifestPluginProcessor.h>
#include <UpdateCheck.h>

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
        "ui": { "width":600, "height":300, "tabs": [ { "name":"main" } ] } } ]
})";

const char *kSite = "https://benjamindehli.github.io/SubC/";

// A product name no instrument has, so its settings file is the test's own.
const char *kProduct = "DMSE Update Check Test";

std::unique_ptr<dm::ManifestPluginProcessor> makeProcessor(const char *site) {
  dm::ManifestPluginProcessor::Assets a;
  a.name = kProduct;
  a.version = "2.1.0";
  a.siteUrl = site;
  a.manifestJson = kManifest;
  a.manifestJsonSize = (int)std::strlen(kManifest);
  a.findResource = [](const juce::String &, int &size) -> const char * {
    size = 0;
    return nullptr;
  };
  return std::make_unique<dm::ManifestPluginProcessor>(std::move(a));
}

template <typename T>
T *find(juce::Component &root, const std::function<bool(T &)> &match) {
  for (auto *child : root.getChildren()) {
    if (auto *t = dynamic_cast<T *>(child))
      if (match(*t))
        return t;
    if (auto *found = find<T>(*child, match))
      return found;
  }
  return nullptr;
}

juce::Button *buttonTitled(juce::Component &root, const juce::String &title) {
  return find<juce::Button>(
      root, [&](juce::Button &b) { return b.getTitle() == title; });
}

class UpdateCheckTests : public juce::UnitTest {
public:
  UpdateCheckTests() : juce::UnitTest("Update check", "engine") {}

  void runTest() override {
    versions();
    feed();
    settings();
    editor();
  }

private:
  // A comparison that comes out backwards nags every player, and one that is
  // too eager treats a malformed feed as an upgrade.
  void versions() {
    beginTest("which version is newer");
    using dm::isNewerVersion;
    expect(isNewerVersion("2.2.0", "2.1.0"), "a higher minor is newer");
    expect(isNewerVersion("3.0.0", "2.9.9"), "a higher major is newer");
    expect(isNewerVersion("2.1.1", "2.1.0"), "a higher patch is newer");
    expect(!isNewerVersion("2.1.0", "2.1.0"), "the same version is not");
    expect(!isNewerVersion("2.0.9", "2.1.0"), "an older version is not");
    expect(isNewerVersion("2.1.10", "2.1.9"),
           "10 is newer than 9, which sorting as text would deny");
    expect(!isNewerVersion("2.1.9", "2.1.10"), "and the reverse");
    expect(isNewerVersion("v2.2.0", "2.1.0"), "a leading v is ignored");
    expect(isNewerVersion("2.2", "2.1.9"), "a missing component is zero");
    expect(!isNewerVersion("2.2", "2.2.0"), "so 2.2 and 2.2.0 are one");
    for (const char *junk : {"", "  ", "latest", "2.2.0-beta", "v", "2..0",
                             "<!doctype html>", "2.2.0a"})
      expect(!isNewerVersion(junk, "2.1.0"),
             juce::String("\"") + junk + "\" is not a newer version");
    expect(!isNewerVersion("3.0.0", "garbage"),
           "an unreadable current version blocks the comparison");
  }

  void feed() {
    beginTest("reading latest.json");
    using dm::parseReleaseJson;
    const auto good = parseReleaseJson(
        R"({"version":"2.2.0","url":"https://benjamindehli.github.io/SubC/"})",
        kSite);
    expect(good && good->version == "2.2.0", "a well-formed feed parses");
    expect(good && good->url == kSite, "and carries the page");

    const auto noUrl = parseReleaseJson(R"({"version":"2.2.0"})", kSite);
    expect(noUrl && noUrl->url == kSite,
           "a feed with no url falls back to the product page");

    // The link is opened in a browser, so only the product page passes.
    for (const char *elsewhere :
         {"https://example.invalid/", "http://benjamindehli.github.io/SubC/",
          "file:///etc/passwd", "javascript:alert(1)",
          "https://benjamindehli.github.io/SubC.evil/",
          "https://benjamindehli.github.io/Omni-84/",
          "https://benjamindehli.github.io/SubC/../Omni-84/"}) {
      const auto moved = parseReleaseJson(
          juce::String(R"({"version":"2.2.0","url":")") + elsewhere + "\"}",
          kSite);
      expect(moved && moved->url == kSite,
             juce::String("a link to ") + elsewhere +
                 " is replaced by the product page");
    }

    for (const char *bad : {"", "{}", "[]", "not json", R"({"version":""})",
                            R"({"version":null})", "null"})
      expect(!parseReleaseJson(bad, kSite).has_value(),
             juce::String("a feed of \"") + bad + "\" yields nothing");

    expect(dm::isUsableSite(kSite), "a product page is a usable site");
    for (const char *bad :
         {"", "http://benjamindehli.github.io/SubC/",
          "https://benjamindehli.github.io/SubC", "https://", "https:///",
          "https://benjamindehli.github.io/../x/"})
      expect(!dm::isUsableSite(bad),
             juce::String("\"") + bad + "\" is not a usable site");
  }

  // The answers are kept in a file, read fresh on every call.
  void settings() {
    beginTest("the answers outlive the object that wrote them");
    dm::UpdateSettings s(kProduct);
    const auto folder = s.file().getParentDirectory();
    const bool folderExisted = folder.isDirectory();

    expect(!s.allowed() && !s.offered(),
           "nothing is allowed or asked at first");
    s.setAllowed(true);
    expect(dm::UpdateSettings(kProduct).allowed(), "allowed is read back");
    s.setAllowed(false);
    expect(!dm::UpdateSettings(kProduct).allowed(), "and so is not allowed");
    s.setOffered(true);
    expect(dm::UpdateSettings(kProduct).offered(), "and having been asked");
    expect(s.file().getParentDirectory().getFileName() == kProduct,
           "the file sits in the instrument's own folder");

    s.file().deleteFile();
    if (!folderExisted)
      folder.deleteRecursively();
  }

  void editor() {
    beginTest("the editor asks once, and shows a release");
    const auto file = dm::UpdateSettings(kProduct).file();
    const auto folder = file.getParentDirectory();
    const bool folderExisted = folder.isDirectory();
    file.deleteFile();

    {
      auto plain = makeProcessor("");
      expect(plain->getUpdateCheck() == nullptr,
             "with no product page there is no update check");
      std::unique_ptr<juce::AudioProcessorEditor> ed(plain->createEditor());
      expect(
          buttonTitled(*ed, "Do not check for a new version") == nullptr ||
              !buttonTitled(*ed, "Do not check for a new version")->isShowing(),
          "and nothing is asked");
    }

    auto p = makeProcessor(kSite);
    expect(p->getUpdateCheck() != nullptr, "a product page gives a check");

    auto question = [](juce::Component &root) {
      return find<juce::Component>(root, [](juce::Component &c) {
        return c.getTitle() == "Check for a new version?";
      });
    };

    {
      std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
      auto *card = question(*ed);
      expect(card != nullptr && card->isVisible(), "the first window asks");
      expect(!dm::UpdateSettings(kProduct).offered(),
             "and opening it marks nothing, so a host scanning its plugins "
             "does not use the question up");
      if (auto *notNow = buttonTitled(*ed, "Do not check for a new version"))
        notNow->onClick();
      expect(card != nullptr && !card->isVisible(), "an answer hides it");
    }
    expect(dm::UpdateSettings(kProduct).offered() &&
               !dm::UpdateSettings(kProduct).allowed(),
           "Not now is remembered, and nothing is allowed");

    for (int open = 0; open < 3; ++open) {
      std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
      auto *card = question(*ed);
      expect(card == nullptr || !card->isVisible(),
             "so later windows do not ask again");
    }
    expect(!dm::UpdateSettings(kProduct).allowed(),
           "and opening windows never turns the check on");

    {
      std::unique_ptr<juce::AudioProcessorEditor> ed(p->createEditor());
      auto *editor = find<dm::ManifestEditor>(
          *ed, [](dm::ManifestEditor &) { return true; });
      expect(editor != nullptr, "the editor is inside the plugin window");
      if (editor != nullptr) {
        editor->announceRelease("2.2.0", kSite);
        auto *link =
            buttonTitled(*ed, "Version 2.2.0 is out. Opens the product page.");
        expect(link != nullptr && link->isVisible(),
               "a release found shows as a link in the strip");
        editor->announceRelease({}, {});
        expect(link != nullptr && !link->isVisible(),
               "and clearing it hides the link");
      }

      // The setting in the Settings panel, read from the file when it opens.
      if (auto *gear = find<juce::TextButton>(*ed, [](juce::TextButton &b) {
            return b.getButtonText() == "Settings";
          }))
        gear->onClick();
      auto *toggle = find<juce::ToggleButton>(*ed, [](juce::ToggleButton &b) {
        return b.getTitle() == "Check for updates";
      });
      expect(toggle != nullptr && !toggle->getToggleState(),
             "the Settings panel shows the setting, off");
    }

    file.deleteFile();
    if (!folderExisted)
      folder.deleteRecursively();
  }
};

UpdateCheckTests updateCheckTests;
} // namespace
