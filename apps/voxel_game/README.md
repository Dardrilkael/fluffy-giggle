# ws voxel — a first-person voxel sandbox

A Minecraft-style voxel sandbox built entirely on the `fluffy-giggle` (ws)
C++20 framework — **no third-party header ever appears in this app's code**
(the workspace rule): Vulkan sits behind `ws::ui`'s r2d/r3d renderers,
Jolt behind `ws::physics3d`, OpenAL Soft behind `ws::audio`.

## What you get

- **A procedurally generated 128 × 64 × 128 voxel world** (8×8 chunks of
  16×64×16): multi-field FBM height and climate maps create plains,
  forests, deserts and rocky mountain regions. A warped low-frequency
  field carves winding river channels and sandy banks; water fills low
  ground to the water plane. 3D coherent density carves underground caves,
  clustered 3D noise creates coal/iron ore veins, and tree density varies
  with forest climate. All fields are deterministic per seed
  (default seed `0xC0FFEE`) and continuous across chunk boundaries.
- **A first-person player**: WASD movement, mouse look, space jump
  (~1.5 m apex — clears a full block with margin), shift sprint. The
  player uses a backend-independent swept-AABB character controller
  against the chunk data directly: no static terrain rigid bodies and no
  player rigid body. Collision work is limited to the player's swept
  local bounds, with axis-separated wall sliding, overlap recovery,
  jump buffering and coyote time. Jolt remains available through
  `ws::physics3d::World3D` for ordinary dynamic objects.
- **Block interaction**: left mouse breaks and collects drops; right mouse places the selected hotbar stack and consumes one item. The DDA raycast has a 7-block reach and placement cannot entomb the player.
- **Expanded blocks**: cobblestone, planks, bricks, glass, leaves, coal ore and iron ore join the original terrain palette. Generated trees have leaf crowns and underground stone contains deterministic coal/iron deposits. New variants reuse the closest existing pack textures with distinct tints.
- **Inventory and crafting**: 27 stackable slots, a 9-slot hotbar, stack limit 64, item counts, and a starter recipe book. E opens inventory, C opens crafting, and F1-F6 craft the listed recipes when materials are available.
- **HUD**: crosshair, a 9-slot item hotbar (keys 1–9), an FPS counter,
  position/grounded readout, and the controls line.
- **Audio**: synthesized place/break "tock" one-shots (in-memory PCM
  through `ws::audio::SoundDesc` — no asset files).

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
`ui`, `physics` or `audio` silently skips the app (the repo's gating
contract).

## The architecture

```
src/world.hpp/.cpp    the voxel data model: the block table (8×8 chunks),
                      seeded biome/climate fields, rivers, caves, ore
                      veins, greedy meshing and a zero-copy VoxelGrid3D
                      view over chunks.
src/inventory.hpp     stackable item inventory, recipe definitions and atomic crafting.\nsrc/player.hpp/.cpp   the FPS character controller: yaw/pitch look,
                      gravity/jump integration, swept-AABB movement,
                      axis-separated sliding and probe-based grounding.
libs/physics/...      backend-independent VoxelGrid3D: bounded occupancy
                      queries, Amanatides-Woo DDA raycast, AABB overlap,
                      swept-AABB collision and penetration recovery.
src/renderer.hpp/.cpp the game→render coordinate bridge: camera and light;
                      draws every chunk mesh.
src/main.cpp          the VoxelApp: input mapping, block interaction,
                      chunk mesh rebuilds, HUD, audio and app entry points.
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
- The water block type exists in the palette but is not meshed or
  swimmable — it only shapes the terrain (sand shorelines).
- Mouse look is free-cursor (window-edge clamped): the shell has no
  pointer-lock seam yet; the deltas are the same, the pointer stays
  visible.
- No chunk streaming: the 8×8 world exists up front. The new 3D cave
  and ore fields add CPU work during startup generation; terrain is still
  generated once rather than streamed.
