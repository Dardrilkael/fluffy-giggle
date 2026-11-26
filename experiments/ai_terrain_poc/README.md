# AI Terrain Generator POC

An isolated C++20 experiment. It has no dependency on the voxel game, renderer,
physics, or third-party libraries. It analyzes synthetic heightmaps and maps
their mean, variance, average neighbor slope, and local roughness to normalized
terrain controls. A seeded hash adds small repeatable variation.

This is a parameter-decision prototype, not a chunk/world generator or a
trained machine-learning model.

From the repository root:

```sh
cmake -S . -B build/ai-terrain-poc -DWS_BUILD_APPS=OFF -DWS_BUILD_EXPERIMENTS=ON -DWS_BUILD_TESTS=ON
cmake --build build/ai-terrain-poc --target ai_terrain_poc
ctest --test-dir build/ai-terrain-poc -R ai_terrain_poc_smoke --output-on-failure
```

Run `build/ai-terrain-poc/bin/ai_terrain_poc` (append `--self-test` to run
the smoke test directly).
