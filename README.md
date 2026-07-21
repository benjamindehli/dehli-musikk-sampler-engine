# dehli-musikk-sampler-engine

A reusable JUCE based sampler engine that loads its own native preset format, a JSON manifest plus FLAC sample bundle, and renders it as audio and a data driven user interface. It is the shared base for the [Dehli Musikk](https://github.com/benjamindehli) sample plugins: the engine is written once, and every sample library becomes a VST3, AU and Standalone plugin without new engine code.

The engine does not read DecentSampler files itself. The companion tool [ds-plugin-converter](https://github.com/benjamindehli/ds-plugin-converter) translates a DecentSampler library into the engine's manifest format at build time, and the manifest can also be authored by hand.

## Features

* Custom polyphonic voice engine with windowed sinc interpolation, velocity layers, round robin selection, loop crossfades, monophonic tag choke and voice steal declicking
* Curved ADSR amplitude envelopes matching DecentSampler's response, per group envelope overrides, one shot playback
* Effect chain: lowpass, highpass, gain, wave shaper (tanh), convolution with runtime impulse response switching, anti phase stereo chorus, phaser and delay, at instrument and per group level
* Note sequencer for auto strum and Omnichord style select and strum playing, where chord keys select and strum keys fire, with seamless voice morphing when the chord changes under ringing notes
* Multiple LFO modulators with waveform shapes, per group tremolo, global and per group tuning modulation
* Data driven editor rendered from the manifest: filmstrip knobs, image buttons, indicator lights, menus, tabs, keyboard color ranges and caption labels, value bubbles, a master output fader and level meter
* Host automation through a generated APVTS parameter layout with bindings compiled at load time, so the audio thread does no string work
* Two sample backends: FLAC embedded in the binary, or a memory mapped sample pack loaded from disk for small binaries and low RAM use
* Lazy per mode decoding, so only the active mode's samples are held in memory
* Mode switching through a lock free pointer swap that is safe while audio runs

## Large libraries

Multi gigabyte libraries do not need a huge binary or a full decode into RAM. By default the converter emits the samples as an external, memory mapped `samples.pak` file rather than compiling them into the plugin, so the binary stays small and the operating system pages in only the slices that are actually used. On top of that, decoding is lazy and per mode: a sample is decoded to PCM the first time a mode acquires it and freed when the last mode using it is retired, so only the active mode's samples are resident. Switching modes releases the previous mode's audio. A plugin can still embed its samples instead (the `EmbeddedFlacSource` backend also serves them from the binary's data), which suits small libraries. The `SampleSource` interface is the seam, so a future fully streamed backend could slot in without touching the voice engine.

## Repository layout

* `source/model/` holds the native data model and the JSON manifest loader and writer. This is the single source of truth for the manifest schema, with lint warnings for unknown keys and dangling references.
* `source/audio/` holds the voice engine, effect chain, note sequencer and sample sources.
* `source/ui/` holds the manifest driven editor components.
* `source/params/` holds the APVTS layout generation and the compiled binding plans.
* `plugin/` holds the shared plugin entry point and the custom standalone app used by every product.
* `cmake/DmsePlugin.cmake` provides `dmse_add_plugin()`, which declares a complete plugin product in a few lines.
* `tests/` holds the console unit tests and their JSON fixtures.

## Building

The engine is a static library consumed with `add_subdirectory` by a parent project that provides the JUCE targets. It is not built standalone. A plugin product is declared like this:

```cmake
dmse_add_plugin(MyProduct
    PRODUCT_NAME "My Product"
    PLUGIN_CODE  Mypr
    VERSION      1.0.0
)
```

That one call sets up the VST3, AU and Standalone formats, the shared entry point, asset embedding, sample pack installation and packaging metadata. If the plugin repository contains `packaging/icon.png`, it is baked into the bundles as the app icon.

## Tests

The parent project builds the test runner when `DMSE_BUILD_TESTS` is on, which is the default:

```
cmake --build build --target dmse_tests
ctest --test-dir build
```

The suite covers the manifest loader and writer round trip, the voice engine, the effect chain, the note sequencer, the compiled parameter plans and the manifest lint.

## Related projects

* [ds-plugin-converter](https://github.com/benjamindehli/ds-plugin-converter) converts DecentSampler libraries into the engine's manifest and asset bundle.
* The Dehli Musikk plugin repositories are thin wrappers that combine this engine with a converted sample library. The finished plugins are paid products, and their sample audio is not part of any public repository.

## License

This project is licensed under the GNU General Public License version 3. See [LICENSE](LICENSE). JUCE is licensed separately by its own terms.
