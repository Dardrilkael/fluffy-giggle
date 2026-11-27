# Task: Validate shared priority-flood drainage routing

Branch: `feature/ai-terrain-generator-poc`

The generator now uses one shared `build_drainage_routing()` implementation for:
- each hydraulic erosion iteration,
- pre-erosion baseline hydrology,
- final post-erosion hydrology.

The graph is built from the current elevation and ocean mask. Ocean cells are outlet seeds; finite-world boundary cells are used as outlets only when no ocean cells exist. D8 directions are constrained to strictly earlier `(filled_elevation, visit_order)` keys, so every route is acyclic. Erosion rebuilds this graph after each hydraulic + thermal iteration.

## Required validation

1. Pull the latest branch and inspect the diff before changing anything.
2. Configure and build the core tests:
   ```powershell
   cmake --preset experiments-debug
   cmake --build build/debug --target test_rgba_terrain_core
   ```
3. Run the complete core test binary:
   ```powershell
   ctest --test-dir build/debug -R "^rgba_terrain_core$" --output-on-failure
   ```
4. Run the focused erosion/drainage regression:
   ```powershell
   ctest --test-dir build/debug -R "whole-world erosion changes terrain" --output-on-failure
   ```
5. Confirm the new regression proves every land routing cell reaches an ocean sink without cycling, while also preserving:
   - zero erosion delta on ocean pixels,
   - non-zero erosion on land,
   - deterministic output,
   - exact chunk stitching.
6. Run the full 256x256 terrain-generation test and inspect `terrain_output/drainage-direction.png`, `flow-accumulation.png`, `river-strength.png`, `lake-depth.png`, and `erosion-delta.png`.
7. Check that runoff reaches ocean outlets, inland drainage does not terminate in arbitrary land sinks when an ocean exists, and erosion does not modify ocean cells.
8. Run the voxel terrain-import tests if the core tests pass.

## Constraints

- Do not tune erosion, river, lake, mountain, or climate parameters.
- Do not weaken or remove the acyclic-route regression.
- Do not alter RGBA v2 channel packing or chunk-based finite-world behavior.
- If a test fails, fix the actual routing/implementation defect and report the exact change. Separate pre-existing failures from regressions.
- Do not claim any test passed unless it was executed locally and the result was observed.

## Completion

Report exact commands, actual pass/fail counts, any pre-existing failures, and generated diagnostic paths. Delete this task file in the same commit that completes the validation task so `AGENT_TASKS` is empty again.
