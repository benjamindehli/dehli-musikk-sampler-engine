#pragma once

// The opt-in check for a newer release of an instrument.
//
// Each instrument's product page publishes latest.json beside it, written by
// ./dmse site with the page. The plugin reads that one file and sends nothing
// else, and only once the player has said yes.

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>

namespace dm {

/** Whether `candidate` names a release later than `current`.

    Both are dotted numbers, with or without a leading v. Missing components
    count as zero. A string that does not parse is never newer. */
bool isNewerVersion(const juce::String &candidate, const juce::String &current);

/** The one release the plugin asks about. */
struct ReleaseInfo {
  juce::String version;
  juce::String url;
};

/** Reads a release description out of the JSON the product page publishes.

    Returns nothing on anything unexpected. The link is opened in a browser, so
    one that does not lead into `site`, the product page, is replaced by the
    page itself. */
std::optional<ReleaseInfo> parseReleaseJson(const juce::String &json,
                                            const juce::String &site);

/** Whether a site address is one the check may ask: an https address ending in
    a slash, so latest.json can be put after it. */
bool isUsableSite(const juce::String &site);

/** The player's answers for one instrument, kept on this machine in a settings
    file beside the instrument's samples, not in the plugin state, so a new
    instance does not ask again.

    Each call opens the file and lets it go. A PropertiesFile is a juce::Timer,
    and holding one in a static deadlocks the host when the binary unloads. */
class UpdateSettings {
public:
  explicit UpdateSettings(juce::String productName);

  bool allowed() const;
  void setAllowed(bool);

  /** Whether the question has been answered. */
  bool offered() const;
  void setOffered(bool);

  juce::File file() const;

private:
  bool flag(const char *key) const;
  void setFlag(const char *key, bool value);

  juce::String product;
};

/** Asks the product page whether there is a newer release.

    Nothing runs and no socket is opened until `start` is called, which the
    editor only does when the setting allows it. The request runs on its own
    thread. */
class UpdateCheck final : private juce::Thread {
public:
  explicit UpdateCheck(juce::String siteUrl);
  ~UpdateCheck() override;

  /** Fetches in the background, unless a fetch is already running. The
      listener is called on the message thread only if a newer release is
      found. */
  void start(const juce::String &currentVersion);

  /** Asks the fetch to give up, without waiting for it.

      For an editor closing: waiting on the message thread for a socket that is
      not answering would hang the window. The owner joins the thread later. */
  void cancel();

  /** The newer release, if the last check found one. */
  std::optional<ReleaseInfo> newerRelease() const;

  /** Called on the message thread when a newer release is found.

      Guarded by the same lock as the result. Whatever it captures must survive
      being called after the editor that set it has gone. */
  void setListener(std::function<void()>);

  const juce::String &site() const noexcept { return siteUrl; }

  /** The most of latest.json the plugin will read. */
  static constexpr size_t kMaxFeedBytes = 4096;

private:
  void run() override;

  const juce::String siteUrl;
  juce::String current;

  mutable juce::CriticalSection lock;
  std::optional<ReleaseInfo> found;
  std::function<void()> listener;
};

} // namespace dm
