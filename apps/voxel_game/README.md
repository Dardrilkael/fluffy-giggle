# ws voxel — a first-person voxel sandbox

A Minecraft-style voxel sandbox built entirely on the `fluffy-giggle` (ws)
C++20 framework — **no third-party header ever appears in this app's code**
(the workspace rule): Vulkan sits behind `ws::ui`'s r2d/r3d renderers,
Jolt behind `ws::physics3d`, OpenAL Soft behind `ws::audio`.

## What you get

- **A procedurally generated 256 × 64 × 256 voxel world** (16×16 chunks of
  16×64×16): the authoritative `rgba_terrain_core::WorldSampler` generates
  the 2D elevation, climate, biome, and globally routed hydrology fields.
  The existing voxel pass adds underground caves, clustered ore veins and
  trees on top of that sampled surface. All generation is deterministic per
  seed (default seed `0xC0FFEE`).
- **World entry screen**: a fresh launch starts with no terrain world generated or loaded. Choose a seed, increment/decrement it or randomize it, then select **Generate / Regenerate PNG**. Terrain sampling runs in the background and writes the authoritative RGBA PNG plus metadata. The result is loaded as a preview behind the menu; it does not enter gameplay. **Load Saved PNG / Preview** loads the previous map without starting the game. **Play World** is enabled only after a map has been previewed and is the only menu action that enters gameplay.
- **A first-person player**: WASD movement, mouse look, space jump
  (~1.5 m apex — clears a full block with margin), shift sprint. The
  player uses a backend-independent swept-AABB character controller
  against the chunk data directly: no static terrain rigid bodies and no
  player rigid body. Collision work is limited to the player's swept
  local bounds, with axis-separated wall sliding, overlap recovery,
  jump buffering and coyote time. Jolt remains available through
  `ws::physics3d::World3D` for ordinary dynamic objects.
- **Block interaction**: left mouse breaks and collects drops; right mouse places the selected hotbar stack and consumes one item. The DDA raycast has a 7-block reach and placement cannot entomb the player.
- **Expanded blocks**: cobblestone, planks, bricks, glass, leaves, coal/iron ore, gravel, clay, snow, ice, sandstone, mud, mossy stone, red sand, basalt and peat. New variants reuse the closest matching resource-pack texture when an exact asset is absent. Leaves use a cut-out alpha pass; glass and ice share a resource-pack tile and are rendered in that same no-depth-write pass. Face culling preserves opaque neighbor faces behind cut-out materials.
- **Inventory and crafting**: 27 stackable slots, a 9-slot hotbar, stack limit 64, item counts, and a starter recipe book. E opens inventory, C opens crafting, and F1-F6 craft the listed recipes when materials are available.
- **HUD**: crosshair, a 9-slot item hotbar (keys 1–9), an FPS counter,
  position/grounded readout, and the controls line.
- **Audio**: synthesized place/break "tock" one-shots (in-memory PCM
  through `ws::audio::SoundDesc` — no asset files).

## Terrain-generation ownership

`libs/rgba_terrain_core/src/world_sampler.cpp` is the single authoritative
2D world generator used by the voxel game's PNG pipeline. It owns gradient
noise, continental and regional terrain, climate, biome selection, and
finite-world hydrology. The game-specific world profile is centralized in
`src/terrain_world_profile.hpp`; its feature scales are expressed in world
units (one block = one unit), not recomputed from image dimensions.

`generate_synthetic_map()` remains available for source compatibility with
older callers and tests. It is only a v1 RGBA adapter over `WorldSampler`;
it no longer owns a second noise, climate, biome, or river algorithm. Its
legacy channel packing is retained, so old consumers can continue reading
the original material and biome/flag fields. New production code should
construct a `WorldSampler` and use `generate_rgba_region()` with the v2
`MaterialFeatureFlags` / `BiomeId` layout.

The production sampler runs deterministic stream-power hydraulic incision
and thermal talus relaxation over the complete finite routing grid before
final D8 flow accumulation. The resulting elevation offsets are interpolated
into the RGBA map, so independently generated chunks share the same eroded
surface. This whole-world pass is used when the routing grid fits the
existing one-million-cell safety limit; oversized worlds retain the bounded
local hydrology fallback without global erosion. Set
`WorldHydrologySettings::erosion_enabled = false` only for baseline
comparisons or intentionally un-eroded test maps.

The voxel-only detail pass is deliberately separate from 2D world generation:
caves, ores, and vegetation are gameplay details, not competing sources for
the production terrain surface, climate, or river network. The older
`World::generate()` implementation remains only as a standalone compatibility
path for headless logic tests and the opt-in benchmark; it is not the normal
gameplay world-generation path.

## Generating and loading worlds

The title screen generates the game's native 256×256 map without an external
script. The seed is editable with the minus/plus buttons or **randomize**.
**Generate New World / Play** writes these authoritative files relative to
the process working directory:

```
worlds/generated-world/terrain.png
worlds/generated-world/terrain.rgba-terrain
```

The PNG uses RGBA terrain v2: R/G are 16-bit height, B stores the surface
material and feature flags, and A stores the full biome ID. The game rebuilds
the voxel volume from those two files and imports it directly.

Each generated world also writes diagnostic preview PNGs under
`worlds/generated-world/diagnostics/`. These are display-only outputs and do
not participate in world loading:

- `temperature.png`, `humidity.png`, `precipitation.png`
- `orographic-lift.png`, `rain-shadow.png`, `ocean-moisture.png`
- `flow-accumulation.png`, `river-strength.png`
- `drainage-direction.png` (eight distinct colors for D8 directions; black
  represents a sink)
- `stream-order.png` (Strahler order), `river-class.png` (stream, river,
  major river), and `watershed-basins.png` (stable outlet-basin labels)
- `river-influence.png` (channel/bank/floodplain influence) and
  `mountain-peak-distance.png` (distance-derived mountain-system influence)
- `lake-depth.png`, `lake-basins.png`
- `erosion-delta.png` (red = hydraulic incision, blue = hydraulic deposition)
- `hydraulic-erosion-delta.png`, `thermal-erosion-delta.png`, and
  `fluvial-terrain-delta.png` (the three terrain-shaping contributions
  visualized separately)

The accumulation, lake-depth, river-influence, and geomorphology-delta
previews use normalization across the entire routing grid, so their values
remain visually consistent when the same world is generated in separate
regions. The watershed, Strahler and river-class layers are categorical
diagnostics; their labels are stable for a given seed and world extent. A separate
`.rgba-voxels` file is not required. Generation stays on a worker thread;
the PNG import and GPU chunk-mesh upload happen on the main/render thread.

On Android, the app's working directory is changed to its private internal
data directory, so the generated files stay inside app storage. The desktop
path is relative to the directory from which `voxel_game` is launched.

## Controls

| Input | Action |
| --- | --- |
| mouse | look (free-cursor deltas — no pointer lock exists in the shell yet) |
| W A S D | move |
| space | jump |
| shift | sprint |
| 1–9 | hotbar select |
| E | inventory |
| C | crafting |
| F1–F6 | craft listed recipe |
| LMB | break block |
| RMB | place block |
| esc | pause menu |

## Texture assets

The atlas is generated from the Minecraft resource-pack files; do not edit
`src/texture_atlas.hpp` by hand. Extract the archive so the directory
contains files such as `grass_block_top.png`, `dirt.png`,
`oak_log.png`, `cobblestone.png`, `coal_ore.png` and `iron_ore.png`.
Then run from the repository root:

```powershell
py scripts/create_atlas.py --assets "C:\\path\\to\\extracted\\textures\\block"
py scripts/create_atlas.py --check
```

With the supplied archive, the generator resolves 15 distinct textures:
grass top/side, dirt, stone, oak log top/side, sand, water, cobblestone,
oak planks, bricks, glass, oak leaves, coal ore and iron ore. It writes the
PNG data and block-to-face table into `src/texture_atlas.hpp`, plus
`download/atlas_preview.png`. It also bakes
Minecraft's temperate-biome tint into the grayscale `grass_block_top`
and `oak_leaves` masks (preserving leaf transparency); without this,
grass tops and leaves appear gray because the game does not yet have a
biome-color shader. Re-run the generator after updating it. The generator prints the resolved
slot manifest so missing or incorrectly selected assets are immediately
visible. The game now has a compile-time guard that rejects an atlas whose block rows no
longer match the `Block` enum.

## Building

The repo's presets, unchanged (CMake ≥ 3.28, Ninja):

```
cmake --preset linux          # or: debug / release / linux-asan
ninja -C build/linux voxel_game
./build/linux/bin/voxel_game
```

Windows: `cmake --preset vs-debug` / `vs-release`, then build the
`voxel_game` target. Android: the NDK preset — the app has an
`android_main` entry (the bounce app's glue pattern); the game is
desktop-first, so on a phone the HUD renders and tap-to-break works
(primary touch bridges to mouse button 0), but look needs the platform's
next pass (drag-look).

Note: the app appears in the configure automatically only when its whole
lib set is configured (`ws_want_voxel_game`); a subset build that omits
`ui`, `physics`, `audio`, `gameplay` or `rgba_terrain_core` silently skips
the app (the repo's gating contract).

## The architecture

```
src/world.hpp/.cpp    the voxel data model and 3D detail pass (caves, ore,
                      vegetation), greedy meshing and a zero-copy
                      VoxelGrid3D view over chunks.
src/terrain_world_profile.hpp
                      shared world-unit scales and output settings for the
                      authoritative WorldSampler pipeline.
src/inventory.hpp     stackable item inventory, recipe definitions and atomic crafting.\nsrc/player.hpp/.cpp   the FPS character controller: yaw/pitch look,
                      gravity/jump integration, swept-AABB movement,
                      axis-separated sliding and probe-based grounding.
libs/physics/...      backend-independent VoxelGrid3D: bounded occupancy
                      queries, Amanatides-Woo DDA raycast, AABB overlap,
                      swept-AABB collision and penetration recovery.
src/renderer.hpp/.cpp the game→render coordinate bridge: camera and light;
                      draws every chunk mesh.
src/main.cpp          the VoxelApp: title/pause menus, input mapping,
                      world generation/loading, block interaction, HUD and audio.
src/terrain_world_io.* the app-owned RGBA v2 PNG writer and PNG-to-world importer.
```

Two coordinate frames, spelled once each (see `world.hpp`'s header
comment): **game space** (right-handed, +Y up, one block = one meter —
physics, player, raycast, the block table) and **render space** (r3d's
+Y-down world — the mesher emits render-space geometry directly, the
camera builder mirrors the player's pose).

## Known limits (the honest list)

- The voxel collision view reads the world's current chunk data on every
  query. Edits become collision-visible immediately; there is no
  collision-mesh rebuild or per-voxel rigid-body lifecycle. Out-of-world
  cells are empty, matching the world's open-void boundary policy.
- Water renders exposed top surfaces for lakes and river channels, but
  does not yet have side-wall rendering, animated flow, swimming or
  buoyancy physics.
- Mouse look is free-cursor (window-edge clamped): the shell has no
  pointer-lock seam yet; the deltas are the same, the pointer stays
  visible.
- No chunk streaming: the 16×16 world exists up front. The new 3D cave
  and ore fields add CPU work during startup generation; terrain is still
  generated once rather than streamed.
