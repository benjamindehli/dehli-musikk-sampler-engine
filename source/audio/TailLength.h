#pragma once

// How long a mode can keep sounding after its input stops: the longest a note
// rings on after note-off, plus the longest impulse response its convolution
// effects can load. Hosts read this through getTailLengthSeconds() to decide
// how long to keep processing after the last note, for example when they
// suspend a silent plugin or bounce a track. Reporting 0 with a reverb in the
// chain lets them cut the reverb off.
//
// It is a ceiling for the mode, not the value of the moment: a Release knob
// counts at its highest setting and every impulse a menu can switch to counts,
// whichever is chosen now. A tail a few seconds too long costs a host a few
// seconds of processing silence. One too short cuts off sound.

#include "SampleSource.h"
#include <model/Manifest.h>

#include <cmath>

namespace dm {

/** The longest value a Release (ENV_RELEASE) binding can set, in seconds, or 0
    when there is no such binding or its range cannot be read. */
inline double longestReleaseBinding(const Binding &b) {
  if (b.parameter != "ENV_RELEASE")
    return 0.0;
  if (b.translation == "linear")
    return juce::jmax(b.translationOutputMin.value_or(0.0),
                      b.translationOutputMax.value_or(0.0));
  if (b.translation == "fixed_value")
    return b.translationValue.isDouble() || b.translationValue.isInt()
               ? (double)b.translationValue
               : 0.0;
  if (b.translation == "table") {
    // "in,out;in,out;..." — the outputs are the second numbers.
    double longest = 0.0;
    for (const auto &pair :
         juce::StringArray::fromTokens(b.translationTable, ";", ""))
      longest = juce::jmax(
          longest,
          pair.fromFirstOccurrenceOf(",", false, false).getDoubleValue());
    return longest;
  }
  return 0.0;
}

/** The ceiling described above, in seconds. `irIds` are every impulse the
    mode can load, and `source` must already hold them and the mode's samples
    decoded. */
inline double modeTailSeconds(const Mode &mode, const juce::StringArray &irIds,
                              const SampleSource &source) {
  auto seconds = [&source](const juce::String &id, int start, int end) {
    const auto *buffer = source.get(id);
    if (buffer == nullptr || buffer->sampleRate <= 0.0)
      return 0.0;
    const int frames = buffer->getNumFrames();
    const int last = end > 0 ? juce::jmin(end, frames) : frames;
    return juce::jmax(0, last - juce::jmax(0, start)) / buffer->sampleRate;
  };

  // How long a note rings on after note-off.
  double note = 0.0;
  for (const auto &g : mode.groups) {
    const double release = g.release.value_or(mode.amp.release);
    const bool releaseTrigger = g.trigger == "release";
    for (const auto &s : g.samples) {
      const bool oneShot =
          !s.ampEnvEnabled.value_or(g.ampEnvEnabled.value_or(mode.amp.enabled));
      if (!releaseTrigger && !oneShot) {
        note = juce::jmax(note, release);
        continue;
      }
      // A release-trigger sample starts at note-off, and a one-shot ignores
      // note-off, so either plays its whole length. Played below its root
      // note it is slower, and so longer.
      double length = seconds(s.source, s.start.value_or(0), s.end.value_or(0));
      if (s.pitchKeyTrack && s.rootNote > s.loNote)
        length *= std::pow(2.0, (s.rootNote - s.loNote) / 12.0);
      note = juce::jmax(note, length);
    }
  }

  // A Release knob can lengthen every envelope release up to its highest value.
  for (const auto &tab : mode.ui.tabs) {
    for (const auto &control : tab.controls)
      for (const auto &b : control.bindings)
        note = juce::jmax(note, longestReleaseBinding(b));
    for (const auto &button : tab.buttons)
      for (const auto &state : button.states)
        for (const auto &b : state.bindings)
          note = juce::jmax(note, longestReleaseBinding(b));
    for (const auto &menu : tab.menus)
      for (const auto &option : menu.options)
        for (const auto &b : option.bindings)
          note = juce::jmax(note, longestReleaseBinding(b));
  }

  // The longest impulse response, which keeps sounding after the dry signal.
  double impulse = 0.0;
  for (const auto &id : irIds)
    impulse = juce::jmax(impulse, seconds(id, 0, 0));

  // A long one-shot pitched far down could claim minutes. Past a minute a
  // host is better off with its own silence detection.
  return juce::jmin(60.0, note + impulse);
}

} // namespace dm
