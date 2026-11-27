# RGBA Terrain Core Roadmap

The following exploratory milestones are intentionally skipped for now so
work can proceed directly to end-to-end reconstruction correctness. They
remain recorded for later reconsideration.

## 1. Terrain continuity — IN PROGRESS
- [x] Replace abrupt river height clamping with a distance-based channel and gradual banks.
- [x] Add slope percentiles, slope histograms and extreme-neighbor coordinates for quantitative review.
- [ ] Add configurable thresholds that distinguish real cliffs from generation or encoding artifacts.
- [x] Keep river masks, water presence and terrain elevation distinct.

## 2. Expanded spatial inspector — IMPLEMENTED
- [x] Add slope percentiles and elevation/slope histograms.
- [x] Report coordinates of extreme horizontal/vertical neighbor differences and steepest samples.
- [x] Measure 8-connected regions for rivers, lakes, water, biomes and feature masks.
- [ ] Distinguish real cliffs from generation or encoding artifacts.

## 3. Diagnostic map visualizations — IMPLEMENTED
- [x] Generate independent previews for elevation, slope, biome, material, water and individual feature masks.
- [x] Generate climate previews for temperature, humidity, precipitation, orographic lift, rain shadow and ocean moisture during sampling.
- [x] Keep previews separate from the authoritative RGBA map.
- [x] Ensure previews never feed back into source data.

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

## 6. Multi-scale finite-world planning — FOUNDATION
- [x] Add a compact plan with independent continental, provincial, ecosystem, sub-biome and habitat scales.
- [x] Add climate- and elevation-aware ecosystem/biome suitability.
- [x] Add compact elliptical regional influences for planner-authored geographic constraints.
- [x] Sample any global coordinate deterministically without allocating the whole world.
- [x] Export small RGBA regions through the existing height/material/biome/flags packing.
- [x] Verify that independently generated adjacent regions produce identical shared-edge data.
- [ ] Add a versioned world-plan serializer and AI planner adapter.
- [ ] Extend chunk reconstruction and game streaming to consume generated regions directly.

## 7. Coupled environmental simulation — FOUNDATION
- [x] Derive temperature from latitude and elevation using a configurable lapse rate.
- [x] Transport moisture deterministically from upwind ocean fetch.
- [x] Couple windward terrain rise to orographic lift, cooling and precipitation.
- [x] Carry leeward ridge exposure into a rain-shadow moisture reduction.
- [x] Feed precipitation back into remaining atmospheric moisture availability.
- [x] Keep all atmospheric fetches in absolute coordinates so independent chunks agree.
- [x] Add deterministic absolute-grid D8 drainage directions.
- [x] Estimate precipitation-weighted local watershed contributions without a world-sized raster.
- [x] Detect local closed depressions as candidate lakes and classify river channels.
- [x] Carve river beds into the existing height channel and reconstruct inland water voxels.
- [ ] Replace bounded local accumulation/depression heuristics with cached regional flow graphs and priority-flood basin filling.
- [ ] Add soil-water storage, infiltration, evaporation and seasonal climate.
- [ ] Replace the compact single prevailing-wind model with authored regional/circulation fields.

## Design constraints
- Deterministic for fixed inputs.
- Preserve source spatial ordering and raw channel values.
- No game, renderer, GPU or third-party image types in public headers.
- Validate plans before sampling; sample only requested regions.
- World feature scales are expressed in absolute world units, not normalized by total world size.
