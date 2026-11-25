# ws voxel — a first-person voxel sandbox

A Minecraft-style voxel sandbox built entirely on the `fluffy-giggle` (ws)
C++20 framework — **no third-party header ever appears in this app's code**
(the workspace rule): Vulkan sits behind `ws::ui`'s r2d/r3d renderers,
Jolt behind `ws::physics3d`, OpenAL Soft behind `ws::audio`.

## What you get

- **A procedurally generated 128 × 64 × 128 voxel world** (8×8 chunks of
  16×64×16): value-noise fbm terrain with grass/dirt/stone layers, sand
  shorelines, water at the shore level, scattered wood trunks.
  Deterministic per seed (seed `0xC0FFEE`).
- **A first-person player**: WASD movement, mouse look, space jump
  (~1.5 m apex — clears a full block with margin), shift sprint — a
  physics3d dynamic Box (0.6 × 1.8 × 0.6)
  against a static **collision shell** (one Box per exposed solid cell,
  ~17k bodies), at a 60 Hz fixed tick. The controller was validated
  headlessly first (`libs/physics/tests/test_voxel_proto.cpp`): rest,
  exact walk speed, and the jump apex are pinned by tests.
- **Block interaction**: left mouse breaks, right mouse places (the
  hotbar's selected type), both through an Amanatides & Woo DDA voxel
  raycast with a 7-block reach. Placement is guarded against entombing
  the player. Edits rebuild the touched chunk's mesh (and a boundary
  block's neighbor chunk).
- **HUD**: crosshair, a 5-slot block hotbar (keys 1–5), an FPS counter,
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
| 1–5 | hotbar select |
| LMB | break block |
| RMB | place block |
| esc | quit |

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
                      seeded fbm terrain, per-(chunk, block-type) chunk
                      meshing (face-culled quads → one r3d::MeshData each),
                      and the DDA voxel raycast.
src/player.hpp/.cpp   the FPS controller: the physics3d Box body, yaw/pitch
                      look, velocity-exact movement (the controller owns
                      the horizontal velocity; gravity owns the vertical),
                      contact-driven grounding.
src/renderer.hpp/.cpp the game→render coordinate bridge: the mirrored
                      camera (game +Y up → r3d's +Y down) and the scene
                      light; draws every chunk mesh.
src/main.cpp          the VoxelApp: the App shell subclass, input mapping,
                      the interaction (raycast + chunk rebuild + physics
                      cell add/remove), the HUD, the audio one-shots, and
                      the desktop/Android entry points.
```

Two coordinate frames, spelled once each (see `world.hpp`'s header
comment): **game space** (right-handed, +Y up, one block = one meter —
physics, player, raycast, the block table) and **render space** (r3d's
+Y-down world — the mesher emits render-space geometry directly, the
camera builder mirrors the player's pose).

## Known limits (the honest list)

- The collision grid is the **shell** — every solid cell with an open
  face (see `World::is_exposed`), re-derived around every edit: digging
  deep and tunneling mid-hill just work. The shell is ~17k bodies for
  the shipped seed (measured; the regression test pins it inside the
  budget, and a full World3D acceptance case creates every shell body
  plus the player's). The one soft spot: the border columns' outboard
  faces are not collision (the void reads solid for the exposure test).
- The water block type exists in the palette but is not meshed or
  swimmable — it only shapes the terrain (sand shorelines).
- Mouse look is free-cursor (window-edge clamped): the shell has no
  pointer-lock seam yet; the deltas are the same, the pointer stays
  visible.
- No chunk streaming: the 8×8 world exists up front (~a second of
  mesh uploads at startup).
