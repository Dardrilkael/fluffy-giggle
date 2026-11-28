# Procedural Terrain Generation Plan

Branch: `feature/ai-terrain-generator-poc`

This document is the working plan for the finite-world terrain generator used by
`apps/voxel_game`. The authoritative implementation is
`libs/rgba_terrain_core/src/world_sampler.cpp`; the RGBA v2 PNG remains the
production hand-off between 2D world generation and voxel reconstruction.

## Design principles

1. Geography is a coupled system, not a collection of independent noise maps.
   Geological structure shapes elevation; elevation and climate influence
   precipitation and snow; runoff follows drainage; drainage and rivers shape
   terrain; changed elevation feeds back into climate and hydrology.
2. Use deterministic, world-coordinate fields. A map generated in one call
   must match the same map generated as independently requested chunks.
3. Keep the finite-world model and RGBA v2 channel contract:
   - R/G: 16-bit normalized elevation
   - B: material and feature flags
   - A: biome ID
4. Never increase erosion strength merely to make rivers easier to see. First
   verify the drainage structure, runoff supply and spatial relationships.
5. Keep physical causes separate in code and diagnostics: rainfall/runoff,
   hydraulic incision and transport, thermal hillslope relaxation, and
   fluvial landform shaping.

## Completed implementation stages

### Stage 1 — One authoritative world sampler

- `WorldSampler` owns the terrain, climate, biome and whole-world hydrology
  pipeline used by the production PNG path.
- The old synthetic-map API is a compatibility adapter, not a second terrain
  generator.
- World-unit feature scales and RGBA v2 semantics are documented in the shared
  world profile and sampler API.

### Stage 2 — Shared whole-world drainage

- One deterministic priority-flood implementation supplies filled elevation,
  D8 directions and a topological order.
- Hydraulic erosion and final hydrology use the same routing construction.
- The graph is rebuilt after each hydraulic iteration.
- Ocean cells are protected outlets; boundary outlets are used when a finite
  world contains no ocean.
- Route edges strictly decrease the flood-order key, so routing is acyclic.
- Ocean interpolation leakage is covered by a regression test.

### Stage 3 — Watersheds, river hierarchy and geomorphology

Implementation is present on this branch; local build and visual validation
are still pending.

- Each land routing cell receives a watershed ID inherited from its terminal
  outlet.
- Runoff is derived from climate precipitation and temperature-dependent
  rain/snowmelt contribution. Basin area alone does not manufacture water.
  This is a deterministic annual-style runoff proxy, not a time-stepped snow
  storage or seasonal hydrology simulation.
- Flow accumulation and Strahler order are propagated along the shared D8
  graph. Equal highest-order tributaries increase the downstream order by one.
- River classes distinguish streams (orders 1–2), rivers (3–4), and major
  rivers (5+).
- Hydraulic incision and sediment transport are separate from thermal
  hillslope relaxation.
- River distance and mountain-peak distance fields control channel, bank,
  floodplain, terrace, and mountain-front alluvial-fan influence.
- Hydraulic, thermal, and fluvial elevation changes have separate diagnostic
  images.
- The game PNG pipeline now writes all new diagnostic layers under
  `worlds/generated-world/diagnostics/`.

## Current next step — Validate geographical quality before tuning

Do not tune parameters yet. First build and inspect the actual generated
world. The validation instructions live in:

`AGENT_TASKS/2026-11-27-watershed-river-geomorphology-validation.md`

The stage is accepted only when the local agent reports:

- core build and focused regression results, separating new failures from
  documented pre-existing failures;
- land-cell watershed coverage and outlet reachability without cycles;
- Strahler order obeying the equal-order confluence rule;
- wet catchments contributing more runoff than comparable dry catchments;
- tributaries converging into larger rivers where the rainfall supply supports
  them;
- channels concentrated near centerlines, with smooth banks and wider
  floodplains only around sufficiently large/high-runoff rivers;
- terraces and alluvial fans attached to the river network and mountain-system
  exits rather than appearing as detached decorative blobs;
- hydraulic, thermal and fluvial deltas independently visible and
  deterministic;
- all ocean cells protected from all three terrain-shaping passes;
- all 14 hydrology diagnostic images matching across split-region generation;
- RGBA v2 and voxel terrain import remaining intact.

### Diagnostic outputs

The game's normal generation path writes these files:

- Climate: `temperature.png`, `humidity.png`, `precipitation.png`,
  `orographic-lift.png`, `rain-shadow.png`, `ocean-moisture.png`.
- Drainage: `flow-accumulation.png`, `drainage-direction.png`,
  `watershed-basins.png`, `lake-depth.png`, `lake-basins.png`.
- Rivers: `river-strength.png`, `stream-order.png`, `river-class.png`,
  `river-influence.png`.
- Geological relationship: `mountain-peak-distance.png`.
- Terrain changes: `erosion-delta.png`, `hydraulic-erosion-delta.png`,
  `thermal-erosion-delta.png`, `fluvial-terrain-delta.png`.

Output directory:
`worlds/generated-world/diagnostics/`

The core 256x256 test writes equivalent images below its
`terrain_output/` directory.

## Planned follow-up stages

### Stage 4 — Sediment budget and depositional validation

Begin only after Stage 3's visual audit is complete.

- Track eroded sediment, transported sediment, deposited sediment, material
  exported to ocean outlets, and any explicitly modeled dissolved fraction.
- Add mass-balance diagnostics per iteration and at basin outlets.
- Verify steep reaches transport sediment while low-gradient reaches deposit
  it.
- Separate channel deposits, floodplain deposits and alluvial-fan deposits in
  the diagnostics where the data supports that distinction.
- Do not tune the global erosion coefficient to hide a sediment accounting
  defect.

### Stage 5 — Mountain systems and watershed-source quality

- Audit mountain-system distance fields against regional uplift, ridge
  strength, elevation and local peak locations.
- Check that headwater streams emerge from plausible high-elevation wet areas,
  while dry highlands do not generate large streams without water supply.
- Check rain-shadow and snowmelt relationships across mountain ranges.
- Improve mountain classification or drainage source placement only when the
  diagnostics identify a concrete mismatch.

### Stage 6 — Controlled parameter calibration

- Tune channel width/depth, floodplain radius, terrace profile and fan spread
  only after route topology and sediment accounting are accepted.
- Use several seeds and world profiles; keep the tests deterministic.
- Preserve stable world-scale behavior as the finite-world size changes.

### Stage 7 — Broader ecological feedback

- Use the accepted hydrology and terrain fields as inputs to soil moisture,
  riparian habitat, wetlands and biome transitions.
- Keep large ecosystems coherent while allowing smaller sub-biomes and local
  habitat variation.
- Validate climate/biome feedback without allowing vegetation classification
  to become an independent source of terrain or water.

## Non-goals for the current validation pass

- No threshold or global erosion-strength increases.
- No RGBA v2 layout changes.
- No replacement of the finite-world/chunk-based generator.
- No conversion to a time-stepped climate or hydrology simulator.
- No detached river, floodplain, terrace or fan noise layers.
