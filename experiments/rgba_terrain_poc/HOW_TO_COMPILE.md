# Build the RGBA terrain generator

From the repository root, run:

    .\experiments\rgba_terrain_poc\build.ps1

For a clean Release build:

    .\experiments\rgba_terrain_poc\build.ps1 -Clean -Configuration Release

The script configures the `rgba_terrain_core` library subset (including its
transitive `core` and `image` dependencies) and this experiment,
disables applications, builds the shared image and terrain-core tests, then runs them.
The first configure may need network access to fetch pinned third-party
dependencies.

## Manual CMake

    cmake -S . -B build/rgba-terrain-poc -DWS_BUILD_APPS=OFF -DWS_BUILD_EXPERIMENTS=ON -DWS_BUILD_TESTS=ON -DWS_BUILD_LIBS=rgba_terrain_core
    cmake --build build/rgba-terrain-poc --config Release --target rgba_terrain_poc test_rgba_terrain_core test_image --parallel
    ctest --test-dir build/rgba-terrain-poc -C Release --output-on-failure -R "^(image($|[.])|rgba_terrain_core($|[.]))"

## Generate a map and voxel volume

    rgba_terrain_poc generate 256 256 0xC0FFEE terrain.png terrain.rgba-terrain terrain.rgba-voxels

The command writes a lossless RGBA8 data PNG, its metadata sidecar, and a
dense RGAVOX02 voxel file. The PNG is reloaded through the strict shared image
library before voxelization, checking the actual persisted map path.

## Inspect the generated map

    rgba_terrain_poc inspect terrain.png terrain.rgba-terrain

This reads the PNG and metadata and reports encoded elevation variation,
neighbor continuity, material/biome counts, and feature coverage without
building a voxel volume.

## Convert an existing map

    rgba_terrain_poc convert input.png input.rgba-terrain output.rgba-voxels

The input must be a lossless 8-bit RGBA PNG. See README.md for channel packing
and output format details.
