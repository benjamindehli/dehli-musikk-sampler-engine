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
//
// A manifest may be authored as ONE file, or SPLIT across a folder for
// readability / hand-editing:
//   manifest/
//     index.json      — schema, format, library, gainDb, and "modes": ["bass", ...]
//     modes/<name>.json  — one file per mode
//     partials/<name>.json — reusable fragments shared by several modes
// A mode (or any node) pulls in partials with:
//   "$use": ["reverb", ...]           — deep-merge whole partials; this node's own fields win
//   { "$ref": "reverb", ...overrides } — splice a partial into an array/field, overriding fields
// `resolveSplitManifest` merges the split form back into a single manifest var,
// which then parses through the identical `loadManifest` path.

#include "Manifest.h"
#include <functional>

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

/** Load a split manifest from a folder (index.json + modes/ + partials/), merge it
    into a single manifest, and parse. */
ManifestParseResult loadManifestFromFolder (const juce::File& manifestDir);

/** Merge a split manifest into a single manifest var. `index` is the parsed
    index.json; `modeLoader`/`partialLoader` return the parsed var for a mode/partial
    by name (a void var means "not found"). Exposed for testing without a filesystem;
    `$use`/`$ref` are resolved and import cycles are reported into `errors`. */
juce::var resolveSplitManifest (const juce::var& index,
                                const std::function<juce::var (const juce::String&)>& modeLoader,
                                const std::function<juce::var (const juce::String&)>& partialLoader,
                                juce::StringArray& errors);

} // namespace dm
