# AI Terrain Generator POC

An isolated C++20 experiment for deciding terrain parameters from normalized
heightmap samples. It is independent of the voxel game, renderer, physics, and
third-party libraries.

The prototype contains a terrain analyzer, an interpretable fuzzy-style
controller, deterministic sequential region simulation, and synthetic flat,
rolling, and mountainous input fixtures. It is a parameter-decision experiment,
not the game's world/chunk generator or a trained machine-learning model.

## Configure and build

From the repository root:

```sh
cmake -S . -B build/ai-terrain-poc -DWS_BUILD_APPS=OFF -DWS_BUILD_EXPERIMENTS=ON -DWS_BUILD_TESTS=ON
cmake --build build/ai-terrain-poc --target ai_terrain_poc ai_terrain_poc_tests
ctest --test-dir build/ai-terrain-poc -R ai_terrain_poc_tests --output-on-failure
```

## Run the experiment

Run from the repository root:

```powershell
.\build\ai-terrain-poc\bin\ai_terrain_poc.exe
```

The executable writes a reproducible run to
`ai_terrain_output/run_seed_123456/` relative to the current working
directory. The seed and region count are currently defined in `src/main.cpp`.

Each sample directory contains:

- `heightmap.bin`: exact row-major float32 samples. Format: 8-byte
  `AITHMAP1` magic, uint32 width, uint32 depth, then width*depth float32
  values, all little-endian.
- `heightmap.bmp`: 24-bit grayscale preview for quick visual inspection.
- `analysis.json`: dimensions and measured terrain features.
- `parameters.json`: controller output for that sample.

The run root contains `manifest.json`, recording the format version, seed,
region count, and sample paths. The `regions/` directory stores each generated
synthetic region together with the features observed by the controller and
the parameter profile used to create it.

The binary heightmap is authoritative; the BMP is only a visualization.
Runs using the same seed and settings are deterministic. The current fixed
output directory is overwritten when the same experiment is run again, so
copy or rename it if you want to preserve multiple runs.
