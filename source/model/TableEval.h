#pragma once

// Shared "in,out;in,out;..." translation-table evaluation (DecentSampler curve
// tables). Piecewise-linear interpolation between the points, clamped to the
// first/last output outside the table's input range.
//
// This string-walking form is for MESSAGE-THREAD, on-change use (UI visibility/
// opacity bindings). The audio path does NOT use it — CompiledMode parses tables
// once into point lists at load and evaluates numerically per block.

#include <juce_core/juce_core.h>

namespace dm
{

inline double evalTableLinear (const juce::String& table, double x)
{
    double prevIn = 0.0, prevOut = 0.0; bool havePrev = false;
    int start = 0;
    while (start < table.length())
    {
        int semi = table.indexOfChar (start, ';');
        if (semi < 0) semi = table.length();
        const int comma = table.indexOfChar (start, ',');
        if (comma > start && comma < semi)
        {
            const double in  = table.substring (start, comma).getDoubleValue();
            const double out = table.substring (comma + 1, semi).getDoubleValue();
            if (x <= in)
            {
                if (! havePrev || ! (in > prevIn)) return out;   // before first point / vertical step
                const double t = (x - prevIn) / (in - prevIn);
                return prevOut + t * (out - prevOut);
            }
            prevIn = in; prevOut = out; havePrev = true;
        }
        start = semi + 1;
    }
    return havePrev ? prevOut : 0.0;   // past the last point → last output
}

} // namespace dm
