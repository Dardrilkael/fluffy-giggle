# ws::rgba_terrain_core — CLAUDE.md

Reusable CPU-side terrain data-map and voxel reconstruction library.

## Responsibilities
- Decode RGBA8 maps through ws::image.
- Parse terrain metadata sidecars and preserve source pixel ordering.
- Generate deterministic synthetic terrain data maps and inspect encoded fields.
- Validate compact finite-world plans, sample ecological/climate fields at global coordinates, and export requested RGBA regions.
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
- World feature scales use absolute world units, independent of total world dimensions.
- Chunk/region output samples global coordinates; independently generated neighboring regions must stitch exactly.
- Diagnostic previews are independent disposable RGBA8 images; never use them as terrain inputs. Climate preview layers are emitted from the same deterministic samples and do not change the authoritative RGBA data map.
- Hydrology uses an absolute world-coordinate D8 grid, fixed-window flow accumulation and local spill-level basin detection; never cache chunk-local flow state or make output depend on generation order.
- Alpha feature bits 0x08/0x10/0x20/0x40 represent forest/mountain/river/lake; 0x80 marks water presence. Inland river/lake water is reconstructed immediately above its encoded channel/basin bed.
- Avoid per-voxel heap allocations.

## Validation
Tests cover decoding, metadata, deterministic maps, column expansion,
voxel material layers, water fill and map analysis.


## Voxel persistence
- RGAVOX02 is a 20-byte header (magic + uint32 width/depth/height, little-endian)
  followed by five explicit bytes per voxel: kind, material, biome, density,
  feature flags.
- Never serialize `sizeof(Voxel)`; its padding/layout is not file format.
- Use `write_voxel_file` / `read_voxel_file` and return ws::core::Status/Result.
- Reject unsupported magic, zero/overflow dimensions, wrong exact file size,
  invalid voxel kind and truncated data.
- Keep the reader/writer bounded in fixed-size blocks.
- Use `compare_voxel_volumes` for exact field-level validation.
