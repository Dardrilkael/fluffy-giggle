# AI Terrain Generator POC

An isolated C++20 experiment for deciding terrain parameters from normalized
heightmap samples. It is independent of the voxel game, renderer, physics, and
third-party libraries.

The current prototype contains:
- A terrain analyzer for mean height, variance, average gradient magnitude,
  local roughness, and directional gradient statistics.
- An interpretable fuzzy-style controller that maps those features to a
  normalized terrain parameter profile.
- Deterministic seeded variation and a sequential region simulation with
  smoothing between neighboring region profiles.
- Synthetic flat, rolling, and mountainous maps used only as test fixtures.

This is a parameter-decision prototype. The synthetic map builder is a test
fixture, not the game's world/chunk generator, and the controller is not a
trained machine-learning model.

## Configure and build

From the repository root:

```sh
cmake -S . -B build/ai-terrain-poc -DWS_BUILD_APPS=OFF -DWS_BUILD_EXPERIMENTS=ON -DWS_BUILD_TESTS=ON
cmake --build build/ai-terrain-poc --target ai_terrain_poc ai_terrain_poc_tests
ctest --test-dir build/ai-terrain-poc -R ai_terrain_poc_tests --output-on-failure
```

## Run the demo

```sh
./build/ai-terrain-poc/bin/ai_terrain_poc
```

On Windows:

```powershell
.\build\ai-terrain-poc\bin\ai_terrain_poc.exe
```

The demo prints feature analysis and parameter profiles for three synthetic
terrain types, followed by a deterministic ten-region parameter sequence.
