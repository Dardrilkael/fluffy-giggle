# RGBA Terrain Core Roadmap

The following exploratory milestones are intentionally skipped for now so
work can proceed directly to end-to-end reconstruction correctness. They
remain recorded for later reconsideration.

## 1. Terrain continuity — SKIPPED
- [ ] Replace abrupt river height clamping with a distance-based channel and gradual banks.
- [ ] Ensure channel carving does not introduce unintended cliffs.
- [ ] Keep river masks, water presence and terrain elevation distinct.

## 2. Expanded spatial inspector — SKIPPED
- [ ] Add slope percentiles and elevation-difference histograms.
- [ ] Report coordinates of extreme neighbor differences.
- [ ] Measure connected regions for rivers, lakes and biome/feature masks.
- [ ] Distinguish real cliffs from generation or encoding artifacts.

## 3. Diagnostic map visualizations — SKIPPED
- [ ] Generate independent previews for elevation, slope, biome, material, water and feature masks.
- [ ] Keep previews separate from the authoritative RGBA map.
- [ ] Ensure previews never feed back into source data.

## 4. Voxel reconstruction validation — CURRENT
- [x] Verify PNG dimensions and row-major X/Z coordinate preservation.
- [x] Verify exact height decoding for supported channel layouts.
- [x] Verify material, biome, water, density and feature metadata per column.
- [x] Verify voxel kind, material, biome, density and feature flags for every voxel.
- [x] Add versioned voxel-file read/write round-trip validation.
- [x] Add a CLI validation command for PNG + metadata + voxel file.
- [x] Add negative tests for malformed dimensions, truncated payloads, trailing data, invalid voxel kinds and field mismatches.

## 5. Voxel-game integration — CURRENT
- [x] Add an explicit adapter from generic material/biome IDs to game content.
- [x] Convert validated volumes into game chunk storage without renderer dependencies.
- [ ] Compare imported and procedural world behavior, verify chunk-boundary continuity and benchmark the larger 256x256 world.

## Design constraints
- Deterministic for fixed inputs.
- Preserve source spatial ordering and raw channel values.
- No game, renderer, GPU or third-party image types in public headers.
- Validate before adding generation features; measure before optimizing.
