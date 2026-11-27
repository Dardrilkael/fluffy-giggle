# Agent task: validate ocean erosion interpolation fix

**Temporary task file. Delete this file in the same commit that completes the work below.**

## Context

The whole-world erosion simulation operates on routing-grid land cells. Before this fix, `sample_elevation_offset()` bilinearly interpolated land elevation offsets at every output pixel, including analytical ocean pixels. This allowed neighboring land offsets to appear as false erosion/deposition in the ocean-side diagnostic and potentially alter coastal output elevations.

The implementation was changed in:

- `libs/rgba_terrain_core/src/world_sampler.cpp`
  - Added `terrain_is_ocean_at()` to evaluate the un-eroded analytical terrain mask.
  - In `WorldSampler::generate_rgba_region()`, only sample the interpolated erosion offset when the original terrain at that output coordinate is land. Original ocean pixels receive exactly zero erosion offset.
- `libs/rgba_terrain_core/tests/test_world_sampler.cpp`
  - Extended the whole-world erosion regression test to identify ocean pixels from an erosion-disabled baseline and assert that their erosion diagnostic RGB values are all zero.

## Required work

1. Review the changes above for correctness and consistency with the existing world-coordinate and sea-level conventions.
2. Build and run the terrain-core test suite from the repository root using PowerShell:

   ```powershell
   cmake --preset experiments-debug
   cmake --build build/debug --target test_rgba_terrain_core
   ctest --test-dir build/debug -R "^rgba_terrain_core$" --output-on-failure
   ```

3. Run the focused regression test as well. First discover its exact registered name if necessary:

   ```powershell
   ctest --test-dir build/debug -N -R "whole-world erosion changes terrain"
   ctest --test-dir build/debug -R "whole-world erosion changes terrain" --output-on-failure
   ```

4. Regenerate the 256x256 diagnostic output by running the existing full-terrain generation test. Confirm that `erosion-delta.png` has no red or blue pixels at ocean coordinates. Use the erosion-disabled baseline or the original analytical terrain mask to identify ocean pixels; do not infer ocean solely from the erosion output.
5. Confirm land erosion remains nonzero and the deterministic/chunk-stitching assertions still pass.
6. If anything fails, investigate and fix the underlying issue without weakening or deleting tests. Preserve RGBA v2, deterministic output, the PNG pipeline, and finite-world/chunk behavior.
7. Report the exact commands executed, their exit codes, test totals/failures, and the ocean-pixel diagnostic result. Do not claim a command passed unless it was actually run.

## Scope limits

- Do not start the drainage-routing redesign yet.
- Do not tune mountain, river, or erosion parameters.
- Do not change the RGBA v2 channel layout.
- Do not replace `WorldSampler`.
- Keep this task limited to the ocean interpolation leakage fix and its validation.

## Cleanup requirement

After completing the implementation review, tests, and diagnostic verification, **delete this temporary Markdown task file in the same final commit** as any necessary fixes. Mention its deletion in the commit summary.
