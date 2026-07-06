// dehli-musikk-sampler-engine — shared plugin entry point.
//
// Compiled into EVERY product target by dmse_add_plugin() (cmake/DmsePlugin.cmake) —
// deliberately NOT part of the engine static library: createPluginFilter must be
// defined once per plugin, and each target has its own generated BinaryData. The
// per-plugin repo supplies only identity (target/product/code/version) in its
// CMakeLists; all behaviour lives in dm::ManifestPluginProcessor.
//
// DMSE_HAS_ASSETS is defined by dmse_add_plugin when the plugin's assets/ folder has
// converter output. Samples normally come from a memory-mapped disk pack whose path
// the engine derives from the product name (see ManifestPluginProcessor).

#include <ManifestPluginProcessor.h>

#if DMSE_HAS_ASSETS
 #include <BinaryData.h>
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    dm::ManifestPluginProcessor::Assets assets;
    assets.name = JucePlugin_Name;
    assets.version = JucePlugin_VersionString;

   #if DMSE_HAS_ASSETS
    // The manifest is embedded SPLIT (manifest/index.json + modes/ + partials/); the
    // shared processor loads it via findResource, so no single manifest.json is needed.
    assets.findResource = [] (const juce::String& filename, int& sizeOut) -> const char*
    {
        for (int i = 0; i < BinaryData::namedResourceListSize; ++i)
            if (filename == BinaryData::originalFilenames[i])
                return BinaryData::getNamedResource (BinaryData::namedResourceList[i], sizeOut);
        return nullptr;
    };
   #endif

    return new dm::ManifestPluginProcessor (std::move (assets));
}
