# Testing the plan-based terrain generator

The easiest end-to-end test is one PowerShell command from the repository root:

~~~powershell
.\experiments\rgba_terrain_poc\test-world.ps1
~~~

It builds the RGBA terrain POC, runs the image/terrain-core tests, generates
a world using WorldSampler (not the legacy synthetic generator), inspects
the RGBA data map, and verifies that the saved voxel file exactly matches a
fresh PNG + metadata reconstruction.

## Generated files and remembered settings

By default, generated world files are written to:

`build\terrain-world-test\`

- `terrain.png` — authoritative RGBA8 data map.
- `terrain.rgba-terrain` — channel and voxel settings.
- `terrain.rgba-voxels` — optional validation artifact.
- `terrain_diagnostics\` — separate elevation, slope, biome, material, water,
  forest, mountain, river and lake preview PNGs.
- `terrain_temperature.png`, `terrain_humidity.png`,
  `terrain_precipitation.png`, `terrain_orographic_lift.png`,
  `terrain_rain_shadow.png`, and `terrain_ocean_moisture.png` — climate
  previews generated during the same sampler pass as the terrain map.

All preview PNGs are disposable visualization outputs. Only
`terrain.png` plus its metadata sidecar are authoritative terrain inputs.
The output directory can be changed per run with `-OutputDir`. For example:

~~~powershell
.\experiments\rgba_terrain_poc\test-world.ps1 -OutputDir "terrain-tests\mountain-world"
~~~

The script remembers the last selected width, depth, and seed in
`build\terrain-world-test-settings.json`. This is local runtime state under
the ignored `build\` directory; it is not committed to Git.

After setting a value once, running the script without those parameters
reuses the saved values. Supplying only one dimension updates that dimension
and keeps the other saved value:

~~~powershell
# Save seed 12345; the next run without parameters reuses it.
.\experiments\rgba_terrain_poc\test-world.ps1 -Seed 12345
.\experiments\rgba_terrain_poc\test-world.ps1

# Change only the width; saved depth and seed remain unchanged.
.\experiments\rgba_terrain_poc\test-world.ps1 -Width 512
~~~

Restore the original defaults (256x256, seed 0xC0FFEE) with:

~~~powershell
.\experiments\rgba_terrain_poc\test-world.ps1 -ResetSettings
~~~

The reset command also runs the normal build/generation/validation flow.
Combine it with `-SkipBuild` to reuse the existing executable.

## Launch the generated world

To also build the game, run its terrain-import test, and launch it with the
generated map:

~~~powershell
.\experiments\rgba_terrain_poc\test-world.ps1 -LaunchGame
~~~

The game importer currently requires a 256x256 map. The script enforces that
when LaunchGame is selected and restores the three terrain environment
variables after the game exits. If remembered dimensions are not 256x256,
set them back explicitly or use `-ResetSettings`.

## Useful options

~~~powershell
# Change the world seed (and remember it)
.\experiments\rgba_terrain_poc\test-world.ps1 -Seed 12345

# Generate a different-size preview map (and remember both dimensions)
.\experiments\rgba_terrain_poc\test-world.ps1 -Width 512 -Depth 512

# Rebuild from a clean POC build directory
.\experiments\rgba_terrain_poc\test-world.ps1 -Clean

# Reuse the existing executable for faster iteration
.\experiments\rgba_terrain_poc\test-world.ps1 -SkipBuild

# Put output somewhere else for this run
.\experiments\rgba_terrain_poc\test-world.ps1 -OutputDir "terrain-tests\mountain-world"
~~~

The POC currently caps generated maps at 1,048,576 pixels because the test
pipeline also builds a 64-block-high voxel volume. Large finite worlds should
be generated region-by-region through the library API, not as one enormous
voxel file.

## What to check

The inspect report includes elevation/slope histograms, slope percentiles,
coordinates for the largest neighbor steps and steepest sample, feature
coverage, and 8-connected component sizes for water, biomes and features.
`validate` should report an exact match between the saved voxel file and
reconstruction from the PNG and metadata.

To regenerate the nine map-layer previews from an existing PNG and metadata:

~~~powershell
rgba_terrain_poc diagnose terrain.png terrain.rgba-terrain terrain_diagnostics
~~~

The climate previews require the WorldSampler pass and are written
automatically by `generate-world`; they cannot be reconstructed from the
RGBA data map alone because temperature, moisture and atmospheric diagnostics
are deliberately not packed into its four channels.

The PNG is a data map, not a colorized preview: do not edit it in an image
editor that may quantize, blend, or color-correct its RGBA values.

For the lower-level commands and RGBA channel layout, see README.md and
HOW_TO_COMPILE.md.
