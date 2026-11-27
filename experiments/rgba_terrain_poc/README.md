# RGBA Terrain Generator POC

A separate experiment for converting purpose-authored RGBA data images into
spatial voxel-column data. It does not use the AI terrain analyzer. Every source
pixel remains at its original (x,z) coordinate; no global feature vector or
tile-statistics reduction is performed.

## Image contract

Use a lossless RGBA PNG. This is a data image, not a photograph. Preserve exact
channel values: disable color-profile conversion, gamma correction, palette
optimization, lossy compression, and editor operations that alter pixel values.
PNG is only the container; terrain semantics come from a per-image channel layout.

Supported channel roles:
- Height8: one 8-bit normalized elevation channel.
- Height16Msb + Height16Lsb: paired channels forming one 16-bit elevation.
- MaterialId: categorical material identifier.
- BiomeId: categorical biome identifier.
- WaterMask: feature mask (0 absent, 255 full).
- Density: per-pixel density or occupancy.
- Unused: channel intentionally not interpreted.

A layout must contain either exactly one Height8 role or exactly one
Height16Msb/Height16Lsb pair. Other roles are optional and can be assigned to
any remaining channel. The C++ ChannelLayout is the metadata representation
for this first increment; a JSON sidecar reader/writer is a follow-up.

## Height conversion

The encoded height is normalized over 255 or 65535 levels and mapped to the
configured world-height range:
world_height = min_height + normalized_height * (max_height - min_height)

The result is rounded to an integer column height. Each input pixel produces
exactly one VoxelColumn, in row-major order, preserving x/z, height, material,
biome, water mask, and density. Renderer-specific chunks and meshes are not
constructed yet.

## Build and test

cmake -S experiments/rgba_terrain_poc -B build/rgba-terrain-poc
cmake --build build/rgba-terrain-poc
ctest --test-dir build/rgba-terrain-poc --output-on-failure

PNG loading is enabled automatically when CMake finds libpng. Without libpng,
the core image representation, channel interpretation, and voxel conversion
still build and test. Install libpng and reconfigure to enable PNG decoding.

## Current scope

This first slice defines configurable channel semantics, validates layouts,
supports 8-bit and 16-bit elevation, optionally decodes RGBA PNGs, and converts
pixels to spatial voxel-column records. Next: a metadata sidecar reader/writer
and an adapter to the game's actual voxel storage. No AI model or ordinary
image depth estimation is involved.
