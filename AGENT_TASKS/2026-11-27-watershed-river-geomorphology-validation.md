# Task: Validate watershed, river hierarchy and fluvial terrain shaping

Branch: `feature/ai-terrain-generator-poc`

This branch now adds precipitation-derived runoff (rain plus temperature-dependent snowmelt), watershed IDs, Strahler stream order, river classes, distance fields, and fluvial landforms. Hydraulic and thermal erosion are separate passes.

## What changed

- Shared priority-flood D8 routing remains the authoritative whole-world routing graph.
- Every land routing cell receives a watershed ID inherited from its terminal ocean outlet (or finite-world boundary when the grid contains no ocean).
- Runoff is initialized from climate precipitation and a temperature-dependent rain/snowmelt conversion. Basin area does not create water.
- Flow accumulation and Strahler order are propagated on the same deterministic acyclic graph.
- River classes are Stream (orders 1-2), River (3-4), and MajorRiver (5+).
- Hydraulic erosion uses precipitation-derived discharge and stream power. Thermal slope relaxation is a separate process.
- River distance fields drive channel cuts, banks, order/runoff-scaled floodplains, terraces, and alluvial fan deposits near mountain-system exits.
- Mountain peak distance is measured from local maxima in the mountain field, including uplifted regional mountain systems.
- RGBA v2 channel packing remains unchanged.
- Hydrology diagnostics now include stream order, river class, watershed basins, river influence, mountain peak distance, separate hydraulic/thermal deltas, and fluvial terrain delta.

## Required validation

1. Pull the latest branch and inspect the diff from the previous validated drainage head (`5edeb1a3a`). Do not change unrelated files.
2. Build the core target:
   ```powershell
   cmake --preset experiments-debug
   cmake --build build/debug --target test_rgba_terrain_core
   ```
3. Run the complete core suite and focused river/erosion regression:
   ```powershell
   ctest --test-dir build/debug -R "^rgba_terrain_core$" --output-on-failure
   ctest --test-dir build/debug -R "whole-world erosion changes terrain" --output-on-failure
   ```
4. Run the full 256x256 terrain generation test. It should emit the terrain map plus 20 diagnostic PNGs under the test's `terrain_output` directory.
5. Inspect these images:
   - `terrain.png`
   - `drainage-direction.png`
   - `flow-accumulation.png`
   - `stream-order.png`
   - `river-class.png`
   - `watershed-basins.png`
   - `river-influence.png`
   - `mountain-peak-distance.png`
   - `erosion-delta.png`
   - `hydraulic-erosion-delta.png`
   - `thermal-erosion-delta.png`
   - `fluvial-terrain-delta.png`
   - Also inspect existing precipitation, lake and river-strength layers.
6. Confirm visually and numerically:
   - land routing cells have watershed IDs and routes terminate at valid outlets without cycles;
   - precipitation-derived flow accumulation is lower in dry catchments than comparable wet catchments; do not infer discharge from basin area alone;
   - Strahler order begins at 1, increases only when at least two tributaries of the same maximum order merge, and otherwise retains the highest incoming order;
   - tributaries converge into larger channels and stream classes are present where discharge supports them;
   - channel incision is concentrated near centerlines, banks transition smoothly, and wider floodplains occur only around higher-order/high-runoff rivers;
   - terraces and mountain-front fans are spatially attached to the actual river network, not independent decorative blobs;
   - thermal and hydraulic deltas are distinct, deterministic and non-zero where expected;
   - all ocean pixels remain unchanged by hydraulic, thermal and fluvial shaping;
   - all 14 hydrology diagnostic images stitch identically across split-region generation.
7. Run voxel terrain import tests if core tests pass:
   ```powershell
   cmake --preset vs-debug
   cmake --build build/vs-debug --config Debug --target voxel_game test_voxel_terrain_import
   ctest --test-dir build/vs-debug -C Debug -R "^voxel_terrain_import$" --output-on-failure
   ```
8. If Debug-only failures persist, repeat the core test binary in Release and distinguish known pre-existing failures from regressions introduced by this work.

## Constraints

- Do not increase global erosion strength to make rivers more visible.
- Do not change climate, river thresholds, channel depths, mountain parameters, or world-plan scales merely to make the regression pass.
- Do not weaken the Strahler, watershed, no-cycle, ocean-protection, deterministic-output, or chunk-stitch tests.
- Preserve the finite-world/chunk-based design and RGBA v2 channel packing.
- Fix actual implementation defects found by compilation, tests or diagnostics. Record exact file and reason for every fix.
- If stream networks are sparse, first report measured precipitation, runoff and accumulation distributions instead of changing thresholds.

## Completion report

Report:
- exact branch head and commit SHA;
- exact configure/build/test commands and observed results;
- pass/fail counts and pre-existing failures;
- output directory and diagnostic image paths;
- observed stream-order range and counts by river class;
- whether dry/wet accumulation behaved as intended;
- visual findings for channel, bank, floodplain, terrace and alluvial-fan geometry;
- any fixes committed.

Delete this task file in the same commit that completes validation so `AGENT_TASKS` is empty again.
