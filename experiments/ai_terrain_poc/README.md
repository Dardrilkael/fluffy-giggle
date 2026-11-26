# AI Terrain Parameter POC

A deliberately isolated C++20 experiment: inspect a 2D heightmap, infer a
terrain profile with transparent fuzzy-style rules, and use that profile as
the parameter proposal for the next synthetic region.

This is **not** the voxel game's world generator. It does not include or link
the game, renderer, physics engine, or any workspace library. The synthetic
terrain functions exist only to feed repeatable examples into this experiment.

## Pipeline

1. **Input** — a row-major heightmap with normalized heights in [0, 1].
2. **Analysis** — mean height, height variance, average gradient magnitude,
   roughness (difference from the local four-neighbor mean), and X/Z gradients.
3. **Decision** — smooth membership ramps and readable rules estimate
   mountain, hill, and flatland tendencies.
4. **Variation** — a seed/region-indexed integer hash adds small, reproducible
   perturbations. Every output field is clamped to [0, 1].
5. **Feedback experiment** — the proposed profile drives the next synthetic
   map; the next iteration analyzes that map. A response blend damps abrupt
   changes between successive profiles.

The controller is deterministic and rule-based. There is no trained model,
LLM, network call, or external dependency.

## Build and run

From this directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/ai_terrain_demo rolling
./build/ai_terrain_demo flat
./build/ai_terrain_demo mountain
```

On Windows, the same commands work from a Developer PowerShell with a C++20
compiler and CMake 3.20+.

## Output parameters

All values are normalized controls, not physical units:

- `amplitude`, `frequency` — broad shape controls for a future synthesizer.
- `mountainStrength`, `hillStrength`, `flatness` — inferred landscape style.
- `roughness`, `variation` — fine detail and controlled diversity.

## Current limitations

This POC uses a compact set of global statistics, not a spatial classifier.
The synthetic next-region generator is intentionally simple and is only a
test harness. It does not promise biome realism, seamless border matching,
erosion, hydrology, or compatibility with the voxel world's current height
and climate fields. Those would be separate experiments after evaluating
whether the controller's decisions are useful.
