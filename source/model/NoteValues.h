#pragma once

// Musical note values for the tempo-synced sequencer — the single table shared by
// the APVTS parameter (labels), the settings panel, and the NoteSequencer (beats,
// including the StrumSpeed knob's note-value snapping). Order: each denomination
// as straight / triplet / dotted.

namespace dm::notevalues
{
    inline constexpr const char* labels[] = {
        "1/4",  "1/4 triplet",  "1/4 dotted",
        "1/8",  "1/8 triplet",  "1/8 dotted",
        "1/16", "1/16 triplet", "1/16 dotted",
        "1/32", "1/32 triplet", "1/32 dotted" };

    // Step length in beats: triplet = 2/3 of straight, dotted = 1.5x.
    inline constexpr double beats[] = {
        1.0,   2.0 / 3.0,  1.5,
        0.5,   1.0 / 3.0,  0.75,
        0.25,  1.0 / 6.0,  0.375,
        0.125, 1.0 / 12.0, 0.1875 };

    // labels/beats indices sorted slowest -> fastest (beats descending): the sweep a
    // StrumSpeed knob scrolls through when tempo synced, so knob-up is always faster
    // regardless of the straight/triplet/dotted interleaving above.
    inline constexpr int speedOrder[] = { 2, 0, 5, 1, 3, 8, 4, 6, 11, 7, 9, 10 };

    inline constexpr int count = 12;
    inline constexpr int defaultIndex = 6;   // 1/16
} // namespace dm::notevalues
