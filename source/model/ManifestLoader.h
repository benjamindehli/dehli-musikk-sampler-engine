#pragma once

// dehli-musikk-sampler-engine — JSON manifest loader.
//
// Parses the engine's native JSON preset manifest into the typed model in
// Manifest.h. This is the ONLY ingestion path: the engine never reads
// DecentSampler `.dspreset` (the converter produces manifests of this shape).
//
// The loader is strict on structure (a top-level object with a "modes" array,
// each mode named) and lenient on optional fields (absent keys fall back to the
// model defaults / std::nullopt). Problems are reported as human-readable strings
// rather than thrown: `errors` is fatal (ok == false), `warnings` is advisory.

#include "Manifest.h"

namespace dm
{

struct ManifestParseResult
{
    bool ok = false;
    PresetLibrary library;
    juce::StringArray errors;     // structural problems; ok == false if non-empty
    juce::StringArray warnings;   // recoverable oddities (e.g. unknown schema fields)
};

/** Parse a manifest from JSON text. */
ManifestParseResult loadManifestFromJson (const juce::String& jsonText);

/** Parse a manifest from an already-parsed JSON value (a JSON object). */
ManifestParseResult loadManifest (const juce::var& root);

} // namespace dm
