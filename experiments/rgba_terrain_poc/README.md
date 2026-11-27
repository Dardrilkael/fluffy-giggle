# RGBA Terrain Generator POC

A standalone C++20 experiment converting purpose-authored RGBA data images into
spatial voxel-column records. It is separate from the AI terrain analyzer.
Every source pixel remains at its original (x,z) coordinate; there is no global
feature-vector or tile-statistics reduction.

## Image and metadata contract

Use a lossless RGBA PNG, not a photograph. Preserve exact channel values:
disable color-profile conversion, gamma correction, palette optimization, and
any editor operation that changes pixel values. Each image has a matching
`.rgba-terrain` sidecar that defines channel meanings.

Supported channel roles: `height8`, `height16_msb`, `height16_lsb`,
`material_id`, `biome_id`, `water_mask`, `density`, and `unused`.
A layout must contain either one Height8 channel or one MSB/LSB pair for 16-bit
height. The remaining roles are independently assignable to any channels.
The example sidecar in this directory is a 16-bit elevation + material + water
layout.

Sidecar syntax is strict key=value:
```
channel.r=height16_msb
channel.g=height16_lsb
channel.b=material_id
channel.a=water_mask
min_height=0
max_height=256
```

Height is normalized over 255 or 65535 levels and mapped to the configured
world-height range. Every pixel produces exactly one voxel-column record,
preserving x/z, height, material, biome, water mask, and density.

## Build and run

PNG decoding uses libpng when found by CMake:
```sh
cmake -S experiments/rgba_terrain_poc -B build/rgba-terrain-poc
cmake --build build/rgba-terrain-poc
ctest --test-dir build/rgba-terrain-poc --output-on-failure
```
Then:
```sh
rgba_terrain_poc input.png input.rgba-terrain output.voxcols
```

The output is a little-endian binary stream: 8-byte `RGBAVX01` magic,
uint32 width, uint32 depth, followed by row-major records containing uint32 x,
uint32 z, uint32 height, then uint8 material, biome, water mask, and density.
This is an intermediate column representation, not yet the game's chunk or
mesh format.

## Current scope

This POC implements configurable channel semantics, 8/16-bit elevation,
strict metadata loading, optional RGBA PNG decoding, and a CLI that writes
spatial voxel-column data. Next integration step is adapting these records to
the game's actual voxel/chunk storage. No AI model or ordinary-image depth
estimation is involved.
