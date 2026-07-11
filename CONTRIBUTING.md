# Contributing

Thank you for your interest in the engine. Bug reports, portability fixes and well argued improvements are welcome. Please open an issue before starting on anything large, so we can agree on the direction before you spend time on it.

## Project setup

The engine is a static library consumed by a parent CMake project that provides JUCE. It does not build standalone, so develop it inside a project that includes it with `add_subdirectory` and declares at least one plugin or the test target. The Dehli Musikk plugin repositories are the reference consumers.

Build and run the tests before and after your change:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target dmse_tests
ctest --test-dir build
```

If your change touches the manifest model, remember that the schema has two consumers: this engine reads manifests and the [ds-plugin-converter](https://github.com/benjamindehli/ds-plugin-converter) writes them, linking against this repository. Build and run that project's `dmse_convert_tests` as well.

## Guidelines

* The audio thread must not allocate, lock or do string work. Parameters flow through atomics and plans compiled at load time. Anything per block or per sample gets measured before and after.
* New manifest fields must round trip: extend the model, the loader, the writer, the lint key list and the kitchensink test fixture together. Additive schema changes do not bump the schema version, changes in meaning do.
* Follow the surrounding code style: JUCE conventions, four space indentation, a space before argument parentheses, comments that explain intent and constraints rather than restating the code.
* The build must stay warning free with the JUCE recommended warning flags on Clang and GCC.
* Add or extend a test for every behavior change. The console suites in `tests/` run without audio hardware and cover the model, the audio path and the compiled parameter plans.

## Commits and pull requests

Keep commits focused and their messages in the imperative mood. In the pull request, describe what changed, why, and how you verified it, including test output. By contributing you agree that your work is licensed under the GNU General Public License version 3, the same license as the project.

## What does not belong here

Sample audio never belongs in this or any related repository. The Dehli Musikk sample libraries are paid products, and the public repositories deliberately contain no audio. Plugin specific behavior belongs in the manifest format or the converter, not in engine special cases.
