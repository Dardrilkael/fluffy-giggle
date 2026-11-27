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
- [ ] Verify PNG dimensions and row-major X/Z coordinate preservation.
- [ ] Verify exact height decoding for supported channel layouts.
- [ ] Verify material, biome, water, density and feature metadata per column.
- [ ] Verify voxel kind, material, biome and density for every voxel.
- [ ] Add versioned voxel-file read/write round-trip validation.
- [ ] Add a CLI validation command for PNG + metadata + voxel file.
- [ ] Add negative tests for malformed dimensions, truncated files and invalid fields.

## 5. Voxel-game integration — PLANNED
- [ ] Add an explicit adapter from generic material/biome IDs to game content.
- [ ] Convert validated volumes into game chunk storage without renderer dependencies.
- [ ] Compare with the existing procedural world and verify chunk-boundary continuity.

## Design constraints
- Deterministic for fixed inputs.
- Preserve source spatial ordering and raw channel values.
- No game, renderer, GPU or third-party image types in public headers.
- Validate before adding generation features; measure before optimizing.
