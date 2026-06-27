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

} // namespace dm
