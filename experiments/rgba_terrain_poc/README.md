# RGBA Terrain Generator POC

This C++20 experiment exercises the reusable `ws::rgba_terrain_core` library, which generates deterministic RGBA terrain data maps,
writes them as lossless PNGs, reloads those PNGs, and expands the exact decoded
pixels into a dense 3D voxel volume. It does not invoke or modify the game app.

## Generate a data map

From the repository root, build with:

    .\experiments\rgba_terrain_poc\build.ps1

Then run:

    rgba_terrain_poc generate-world 256 256 0xC0FFEE terrain.png terrain.rgba-terrain terrain.rgba-voxels

The command produces three files:
- `terrain.png`: a machine-authored RGBA8 data map, not a colorized preview
- `terrain.rgba-terrain`: explicit channel and voxel-generation metadata
- `terrain.rgba-voxels`: the resulting dense 3D voxel volume

The `generate-world` command uses the plan-based WorldSampler: multi-scale
continents, regional terrain influences, coupled temperature/moisture,
precipitation, D8 drainage, bounded watershed accumulation, river carving and
local lake candidates. It is deterministic for a given size and seed.

The older `generate` command remains available as a legacy synthetic-map
baseline; it does not exercise the plan-based environmental or hydrology
simulation. Use `generate-world` when testing the new generator.

## Load the map in the voxel game

The game needs only the PNG and its metadata sidecar. It reconstructs the
voxel volume directly at startup; the generated `.rgba-voxels` file is not a
required runtime input.

```powershell
$env:WS_VOXEL_TERRAIN_PNG = "C:\terrain\terrain.png"
$env:WS_VOXEL_TERRAIN_META = "C:\terrain\terrain.rgba-terrain"
```

Launch `voxel_game` with those variables set. To additionally validate a
previously generated voxel artifact against the PNG, optionally set
`WS_VOXEL_TERRAIN_VOXELS` to its path. The game still imports the volume
reconstructed from the PNG, never the optional artifact.

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
| A bit 6 | Lake flag |
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
mountain/river/lake flags. Elevation and neighbor differences are calculated from
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

## Plan-based finite-world sampling

The reusable terrain core also exposes an early plan-based sampler API:
`WorldPlan` describes finite world bounds, independent absolute scales for
continents, provinces, major ecosystems, sub-biomes and local habitats, climate
parameters, compact elliptical regional influences, and climate suitability
rules for ecosystems and biomes.

`WorldSampler` validates and owns a plan, then samples any global X/Z
coordinate without allocating the full world. Its `generate_rgba_region`
method emits only the requested rectangle, so a game can generate one chunk
at a time. Adjacent regions use the same global coordinate fields and are
covered by an exact pixel-stitching test.

This is a deterministic procedural foundation, not an AI connection yet.
A future planner can populate the same structures; world-plan serialization,
AI integration, direct chunk voxel reconstruction and game streaming remain
separate follow-up steps. The existing PNG channel layout and reconstruction
functions are unchanged.

## Current limitations

This is a controlled synthetic map generator, not the planned AI terrain
generator. Caves, ores, detailed vegetation, erosion, globally complete watershed
accumulation, priority-flood basin filling, chunk conversion, and meshing are
not implemented in this stage. See `../../libs/rgba_terrain_core/ROADMAP.md` for the current milestone, skipped
steps and planned voxel-game integration.
