#pragma once

// dehli-musikk-sampler-engine — JSON manifest writer.
//
// Serializes the typed model (Manifest.h) back to the engine's native JSON
// manifest. This is the inverse of ManifestLoader and the reason the engine is
// the single source of truth for the schema: ds-plugin-converter links the engine
// and calls this to emit manifests, the engine reads them back with the loader.
//
// Round-trips with the loader for every field the loader understands. The opaque
// `raw` vars kept by Binding/Effect are NOT re-emitted — the typed fields are the
// authoritative representation written here.

#include "Manifest.h"

namespace dm
{

/** Serialize a manifest to a JSON value (a JSON object). */
juce::var manifestToVar (const PresetLibrary& library);

/** Serialize a manifest to JSON text. `oneLine` packs it without pretty-printing. */
juce::String writeManifestToJson (const PresetLibrary& library, bool oneLine = false);

/** Write the manifest as a readable SPLIT folder:
      <manifestDir>/index.json         — schema/format/library/gainDb + ordered mode names
      <manifestDir>/modes/<name>.json  — one file per mode
    Per-mode file names come from each mode's name (slugified, de-duplicated). The
    index lists them in order, so load order is preserved regardless of naming.
    Round-trips with `loadManifestFromFolder`. Returns false if a file couldn't be
    written. (Partials/ is a hand-authoring feature; the converter does not extract
    shared fragments automatically.) */
/** NOTE: one-way for hand-authored structure — the loader resolves $use/$ref
    partials into a flat model, and this writes index.json + modes/ WITHOUT
    re-creating partials/. Never machine-rewrite a hand-authored split manifest
    in place: the $use/$ref organisation would be permanently inlined. */
bool writeSplitManifest (const PresetLibrary& library, const juce::File& manifestDir);

} // namespace dm
