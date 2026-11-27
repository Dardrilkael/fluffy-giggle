# RGBA Terrain Generator POC

A standalone C++20 experiment that turns a purpose-authored RGBA data image
into a real, dense 3D voxel volume. It is separate from the AI terrain
analyzer and does not modify or invoke the game application. Every source
pixel remains at its original (x,z) coordinate; there is no global
feature-vector or tile-statistics reduction.

## Image and metadata contract

Use a lossless, 8-bit RGBA PNG, not a photograph. Preserve exact channel
values: disable color-profile conversion, gamma correction, palette
optimization, and any editor operation that changes pixel values. The
experiment loads images through the shared ws::image library, whose strict
RGBA entry point rejects sources that are not encoded as four channels.

Each image has a matching .rgba-terrain sidecar that defines channel meanings.
Supported roles: height8, height16_msb, height16_lsb, material_id, biome_id,
water_mask, density, and unused. A layout must contain either one Height8
channel or one Height16 MSB/LSB pair. Other roles can be assigned to any
remaining channels, at most once each.

Example sidecar:

    channel.r=height16_msb
    channel.g=height16_lsb
    channel.b=material_id
    channel.a=water_mask
    min_height=0
    max_height=46
    volume_height=64
    sea_level=12
    subsurface_depth=4
    water_threshold=128

Height is normalized over 255 or 65535 levels and mapped into the configured
world-height range. The top height is inclusive: a height of 10 creates solid
voxels at y=0 through y=10.

Optional metadata settings control volume height, sea level, water threshold,
subsurface depth, and default/top/subsurface/deep/water material IDs. Defaults
are defined in TerrainSettings in the public header. The volume height must
be greater than max_height; sea_level must be inside the volume.

## Voxel volume model

The generator performs two explicit stages:

1. Decode each image pixel into a spatial column containing x/z, surface
   height, material ID, biome ID, water mask, and density.
2. Expand those columns into a dense 3D volume. Each voxel stores four bytes:
   kind (air/solid/water), material ID, biome ID, and density.

Solid columns get a surface material at the top, a configurable subsurface
band immediately below it, and a deep material underneath. If the water mask
meets its threshold, water fills empty cells above the terrain through sea
level. Air cells remain zero-initialized. Biome and density data are preserved
on solid cells; water cells receive full density.

Voxel indexing is X-fastest, then Z, then Y:
`x + z*width + y*width*depth`. This makes each horizontal layer contiguous,
which is convenient for image-to-volume generation and later mesh extraction.
The current output is a dense volume, not chunk storage; sparse storage and
streaming are deliberately deferred.

## Build

See HOW_TO_COMPILE.md or run build.ps1 from the repository root:

    .\experiments\rgba_terrain_poc\build.ps1

The script builds only the core + image libraries and this experiment. It
disables applications, then builds and runs both image-decoder and terrain
tests. It does not compile or modify other applications.

## Run

    rgba_terrain_poc input.png input.rgba-terrain output.rgba-voxels

The output is little-endian `RGAVOX01` v1: 8-byte magic, uint32 width, depth,
and height, followed by four bytes per voxel (kind, material, biome, density)
in X-fastest, then Z, then Y order.

## Current scope and next steps

This stage generates a complete 3D voxel volume from the RGBA spatial fields.
It does not yet generate caves, ore, trees, erosion, or hydrological river
networks; those should be separate passes that consume the same spatial
fields instead of distorting or discarding image information. It also does
not yet convert the dense volume into the game's chunk format or mesh.

The next integration step is a tested adapter from this volume into the
existing chunk/block representation, followed by chunk-boundary and memory
considerations. The game runtime and its current procedural generator remain
unchanged.
