# RGBA Terrain Generator POC

This C++20 experiment exercises the reusable `ws::rgba_terrain_core` library, which generates deterministic RGBA terrain data maps,
writes them as lossless PNGs, reloads those PNGs, and expands the exact decoded
pixels into a dense 3D voxel volume. It does not invoke or modify the game app.

## Generate a data map

From the repository root, build with:

    .\experiments\rgba_terrain_poc\build.ps1

Then run:

    rgba_terrain_poc generate 256 256 0xC0FFEE terrain.png terrain.rgba-terrain terrain.rgba-voxels

The command produces three files:
- `terrain.png`: a machine-authored RGBA8 data map, not a colorized preview
- `terrain.rgba-terrain`: explicit channel and voxel-generation metadata
- `terrain.rgba-voxels`: the resulting dense 3D voxel volume

The map is deterministic for a given size and seed. Its current synthetic fields
include broad continental variation, hills, ridged mountain regions, simple
temperature/moisture biome classification, and a meandering river corridor.
The river is a deliberately simple synthetic mask and height cut, not a
hydrological drainage simulation.

## RGBA channel packing

The generated PNG uses every channel:

| Channel | Data |
|---|---|
| R | 16-bit surface height, most-significant byte |
| G | 16-bit surface height, least-significant byte |
| B | Surface material ID |
| A bits 0..2 | Biome ID |
| A bit 3 | Forest flag |
| A bit 4 | Mountain flag |
| A bit 5 | River flag |
| A bit 6 | Reserved |
| A bit 7 | Water flag |

Height is stored as a normalized unsigned 16-bit value and mapped to the
world-height range in the sidecar. The generated map currently uses height
0..56 in a 64-voxel-high volume. PNG and metadata are written to disk and then both are reloaded before
voxel generation. This exercises the image decoder and sidecar parser, so the
voxel stage consumes the persisted channel values and settings.

The metadata role `biome_water_flags` decodes alpha into the biome ID and
feature flags. It cannot be combined with separate `biome_id` or
`water_mask` roles in the same layout.

## Voxel volume

Each source pixel keeps its exact X/Z location and becomes one terrain column.
The volume generator fills solid voxels from the base through the inclusive
surface height, applies surface/subsurface/deep material IDs, and fills water
above flagged low terrain through sea level.

Each voxel is five bytes: kind (air/solid/water), material ID, biome ID,
density, and feature flags. Indexing is X-fastest, then Z, then Y:
`x + z*width + y*width*depth`.

The binary file is little-endian `RGAVOX02`: 8-byte magic, uint32 width,
depth, and height, then five bytes per voxel in the same X/Z/Y order. The
format is encoded field-by-field and does not depend on C++ struct padding.
The library reader rejects unknown versions, zero/overflowing dimensions,
truncated payloads, trailing bytes, and invalid voxel-kind values.

## Validate voxel reconstruction

    rgba_terrain_poc validate terrain.png terrain.rgba-terrain terrain.rgba-voxels

The command reloads the PNG and metadata, reconstructs the expected volume,
loads the saved RGAVOX02 file, and compares dimensions and every voxel field.
It reports mismatch counts by field and the first differing X/Y/Z coordinate.
Exit codes: 0 = exact match, 1 = input/file I/O or parsing error, 2 = usage
error, 3 = valid voxel file but reconstruction mismatch.

## Inspect a data map

    rgba_terrain_poc inspect terrain.png terrain.rgba-terrain

Inspection reports the encoded elevation range, mean and standard deviation,
number of distinct heights, horizontal/vertical neighbor-step statistics,
surface material and biome distributions, and coverage of water/forest/
mountain/river flags. Elevation and neighbor differences are calculated from
the original 8-bit or 16-bit channel values, before conversion to voxel heights.
This is useful for checking whether the image still contains fine spatial
variation rather than only a small set of quantized height levels.

## Convert an existing data map

    rgba_terrain_poc convert input.png input.rgba-terrain output.rgba-voxels

Input must be an 8-bit RGBA PNG. The strict image loader rejects RGB,
grayscale, and 16-bit-per-channel inputs rather than silently changing them.
For 16-bit height, pack the high and low bytes into separate RGBA channels.

## Build and tests

The implementation lives in `libs/rgba_terrain_core` and is registered as
`ws::rgba_terrain_core`. The experiment executable is a thin `ws_add_app`
consumer. The PowerShell script configures the `rgba_terrain_core` subset
(which transitively includes `ws::core` and `ws::image`), builds the executable
and registered library tests, and does not compile or modify other applications.
See HOW_TO_COMPILE.md for the manual CMake commands.

## Current limitations

This is a controlled synthetic map generator, not the planned AI terrain
generator. Caves, ores, vegetation, erosion, drainage simulation, chunk
conversion, and meshing are not implemented in this stage. See `../../libs/rgba_terrain_core/ROADMAP.md` for the current milestone, skipped
steps and planned voxel-game integration.
