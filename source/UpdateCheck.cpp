#include "UpdateCheck.h"

// For MessageManager::callAsync and PropertiesFile. Kept out of the header so
// it stays on juce_core alone.
#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

namespace dm {

namespace {
/// Splits a dotted version into its numbers. An empty result means the string
/// was not a version.
juce::Array<int> parts(const juce::String &raw) {
  juce::Array<int> out;
  auto text = raw.trim();

  if (text.startsWithIgnoreCase("v"))
    text = text.substring(1);

  for (const auto &piece : juce::StringArray::fromTokens(text, ".", "")) {
    if (piece.isEmpty() || !piece.containsOnly("0123456789"))
      return {};

    out.add(piece.getIntValue());
  }

  return out;
}

const char *const kAllowed = "updateCheckAllowed";
const char *const kOffered = "updateCheckOffered";
} // namespace

bool isNewerVersion(const juce::String &candidate,
                    const juce::String &current) {
  const auto a = parts(candidate);
  const auto b = parts(current);

  if (a.isEmpty() || b.isEmpty())
    return false;

  for (int i = 0; i < juce::jmax(a.size(), b.size()); ++i) {
    // A missing component is zero.
    const int x = i < a.size() ? a[i] : 0;
    const int y = i < b.size() ? b[i] : 0;

    if (x != y)
      return x > y;
  }

  return false;
}

bool isUsableSite(const juce::String &site) {
  return site.startsWith("https://") && site.endsWithChar('/') &&
         !site.contains("..") && site.length() > 9;
}

std::optional<ReleaseInfo> parseReleaseJson(const juce::String &json,
                                            const juce::String &site) {
  const auto parsed = juce::JSON::parse(json);

  if (!parsed.isObject())
    return std::nullopt;

  ReleaseInfo info;
  info.version = parsed.getProperty("version", {}).toString().trim();
  info.url = parsed.getProperty("url", {}).toString().trim();

  if (info.version.isEmpty())
    return std::nullopt;

  // The link is opened in a browser, so it may only lead into the product
  // page, and a ".." could climb out of it. Anything else, or none, falls
  // back to the page itself.
  if (!info.url.startsWith(site) || info.url.contains(".."))
    info.url = site;

  return info;
}

UpdateSettings::UpdateSettings(juce::String productName)
    : product(std::move(productName)) {}

juce::File UpdateSettings::file() const {
  // The folder the instrument's samples are installed to (see
  // ManifestPluginProcessor), so everything it keeps is in one place.
  auto dir =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
#if JUCE_MAC
  dir = dir.getChildFile("Application Support");
#endif
  return dir.getChildFile("DehliMusikk")
      .getChildFile(product)
      .getChildFile("Settings.xml");
}

bool UpdateSettings::flag(const char *key) const {
  juce::PropertiesFile::Options o;
  o.storageFormat = juce::PropertiesFile::storeAsXML;
  juce::PropertiesFile props(file(), o);
  return props.getBoolValue(key, false);
}

void UpdateSettings::setFlag(const char *key, bool value) {
  juce::PropertiesFile::Options o;
  o.storageFormat = juce::PropertiesFile::storeAsXML;
  juce::PropertiesFile props(file(), o);
  props.setValue(key, value);
  props.saveIfNeeded();
}

bool UpdateSettings::allowed() const { return flag(kAllowed); }
void UpdateSettings::setAllowed(bool allow) { setFlag(kAllowed, allow); }
bool UpdateSettings::offered() const { return flag(kOffered); }
void UpdateSettings::setOffered(bool offered) { setFlag(kOffered, offered); }

UpdateCheck::UpdateCheck(juce::String site)
    : juce::Thread("Update check"), siteUrl(std::move(site)) {}

UpdateCheck::~UpdateCheck() {
  // The wait must outlast the fetch's five second timeout. If it expires,
  // stopThread kills the thread, which can leave allocator or socket locks
  // held and hang the message thread. In practice cancel() has been called
  // first and the fetch stops within one read.
  stopThread(10000);
}

void UpdateCheck::cancel() { signalThreadShouldExit(); }

void UpdateCheck::setListener(std::function<void()> fn) {
  const juce::ScopedLock sl(lock);
  listener = std::move(fn);
}

void UpdateCheck::start(const juce::String &currentVersion) {
  if (!isUsableSite(siteUrl) || isThreadRunning())
    return;

  current = currentVersion;
  startThread(juce::Thread::Priority::background);
}

std::optional<ReleaseInfo> UpdateCheck::newerRelease() const {
  const juce::ScopedLock sl(lock);
  return found;
}

void UpdateCheck::run() {
  const juce::URL url(siteUrl + "latest.json");

  auto stream = url.createInputStream(
      juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
          .withConnectionTimeoutMs(5000)
          .withNumRedirectsToFollow(3));

  if (stream == nullptr || threadShouldExit())
    return;

  // Read in pieces, so the memory held is bounded and cancel() takes effect
  // within one chunk.
  juce::MemoryOutputStream body;

  while (body.getDataSize() < kMaxFeedBytes) {
    if (threadShouldExit())
      return;

    char chunk[1024];
    const auto got = stream->read(chunk, static_cast<int>(sizeof(chunk)));

    if (got <= 0)
      break;

    body.write(chunk, static_cast<size_t>(got));
  }

  if (threadShouldExit())
    return;

  const auto info = parseReleaseJson(body.toString(), siteUrl);

  if (!info.has_value() || !isNewerVersion(info->version, current))
    return;

  {
    const juce::ScopedLock sl(lock);
    found = info;
  }

  // Taken under the lock, since the message thread may replace it while this
  // runs. The window that set it can close before this thread is joined, so
  // the listener may be called after the window has gone, and has to check.
  std::function<void()> callback;

  {
    const juce::ScopedLock sl(lock);
    callback = listener;
  }

  if (callback != nullptr)
    juce::MessageManager::callAsync([callback] { callback(); });
}

} // namespace dm
