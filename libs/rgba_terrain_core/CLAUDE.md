# ws::rgba_terrain_core — CLAUDE.md

Reusable CPU-side terrain data-map and voxel reconstruction library.

## Responsibilities
- Decode RGBA8 maps through ws::image.
- Parse terrain metadata sidecars and preserve source pixel ordering.
- Generate deterministic synthetic terrain data maps and inspect encoded fields.
- Decode pixels into X/Z terrain columns and expand them into dense voxel volumes.

## Boundaries
- No dependency on the voxel game, renderer, GPU, platform or application.
- Material and biome IDs are opaque bytes; consumers define their semantics.
- Image decoding stays behind ws::image. No stb types in public headers.
- Public headers live in include/ws/rgba_terrain_core and use workspace APIs.

## Invariants
- Preserve image X/Z and row-major ordering.
- Do not smooth, resample, gamma-correct or otherwise transform data-map channels.
- Deterministic output for fixed seed, dimensions and settings.
- Avoid per-voxel heap allocations.

## Validation
Tests cover decoding, metadata, deterministic maps, column expansion,
voxel material layers, water fill and map analysis.
