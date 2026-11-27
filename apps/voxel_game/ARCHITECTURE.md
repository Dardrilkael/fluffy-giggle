# Voxel game: reusable library boundaries

The app is a composition layer. Algorithms and policies that can stand alone
live in the workspace libraries; the app retains the block vocabulary,
resource pack, world rules, and rendering composition.

The voxel world's ownership boundary is CPU-only: `World` owns block storage
and builds renderer-independent CPU mesh snapshots. `VoxelRenderer` owns GPU
handles and performs upload/destruction/draw work on the render thread. The
CPU mesher can therefore be tested and later scheduled on workers without an
active graphics device.

## Shared code

| Responsibility | Library API | Design / hot-path contract |
|---|---|---|
| Character movement | `ws::physics3d::CharacterController` | Direct C++ type, no wrapper object or virtual dispatch. Collision uses the existing zero-copy `VoxelGrid3D` view. |
| Voxel traversal and swept collision | `ws::physics3d::VoxelGrid3D` | Existing function-pointer occupancy seam; no world copy and work stays local to the ray or swept bounds. |
| Chunk storage | `ws::gameplay::ChunkedGrid3D` | One contiguous allocation, chunk spans, compile-time dimensions, direct cell indexing. |
| Inventory and crafting | `ws::gameplay::Inventory` | Fixed `std::array` storage and template item types; crafting is transactional and allocation-free. |
| Procedural fields | `ws::math::noise` | Inline deterministic hash, value noise and FBM helpers. Terrain parameters remain game-owned. |
| Frustum extraction / AABB culling | `ws::math::culling` | Inline plane math and support-vertex tests; no renderer dependency. |
| Menu, hotbar and settings geometry | `ws::ui::layout` | `constexpr` layouts and half-open hit tests; no input polling or drawing in the policy layer. |
| Clock conversion | `ws::core::time` | Inline steady-clock duration conversion. |
| Early-reflection DSP | `ws::audio::EarlyReflectionProfile` | Fixed maximum of eight taps, caller-owned PCM buffers, no heap allocation or audio-device dependency in the mixer. |
| Transient processed sounds | `AudioEngine::play(SoundDesc)` | Copies PCM to an owned backend buffer; transient buffers are recycled after warm-up. |

## Kept in the app intentionally

`Block`, `Item`, the starter recipes, terrain/biome parameters, river/cave/ore/tree rules, material reflectivity mapping, atlas manifest and PNG assets, greedy mesh policy for that atlas, chunk render ordering, scene light, camera coordinate transform, HUD, pause/settings/inventory state, and scripted voxel benchmark phases remain under `apps/voxel_game`. They define this game's content and presentation rather than general-purpose services.

The sound adapter (`voxel_audio.hpp`) is deliberately the boundary between the two layers: it samples nearby voxel geometry and turns the results into generic reflection taps. `ws::audio` never includes or knows about `World` or `Block`.

## Performance constraints

- No `std::function`, virtual interface, runtime plugin dispatch, or per-cell heap allocation was introduced into the movement, storage, inventory, layout, noise, frustum, or PCM-mixing paths.
- Templates and inline helpers preserve compile-time specialization. The controller remains an ordinary direct method call.
- Terrain generation iterates chunk-first, obtains one span per chunk, and writes in the same chunk-major cell order used by meshing.
- Geometry probes and PCM reflection mixing run only when a block sound is played, not on every render frame. Listener updates remain in the existing audio update path.
- Frustum visibility is computed once per chunk per frame and reused by every material mesh belonging to that chunk.
- Chunk meshing classifies exposed faces and tile IDs in one voxel traversal; greedy sweeps consume the cached face/tile data without rechecking neighbors or rescanning by block type.
- This is a structural performance design, not a measured benchmark claim. Build, test, and FPS comparison are still required on the target machine.

## RGBA terrain import

The app owns the only adapter between `ws::rgba_terrain_core`'s generic
voxel IDs and this game's `Block`/`Biome` vocabulary. The reusable terrain
library does not depend on the game. The fixed world is 256x256x64 (16x16
chunks), matching the current 256x256 data-map resolution without resampling.

The import preflights dimensions, voxel kinds, supported material IDs, biome
IDs, full/empty density and column-consistent feature metadata before clearing the world. Generic
material IDs are explicitly mapped in `terrain_import.cpp`; biome and feature
flags are retained in per-column metadata, not discarded or misrepresented as
block types. The renderer remains unchanged and consumes the same World API.


To load a data-map world at startup, set the two authoritative inputs
before launching `voxel_game`:

```powershell
$env:WS_VOXEL_TERRAIN_PNG = "C:\terrain\terrain.png"
$env:WS_VOXEL_TERRAIN_META = "C:\terrain\terrain.meta"
```

The app decodes the PNG, reconstructs the voxel volume from its pixels and
metadata, and imports that in-memory volume directly. No persisted voxel file
is required. With none of the terrain variables set, the original seeded
procedural world remains the default.

An optional persisted voxel artifact can be checked against a fresh
reconstruction for debugging or regression validation:

```powershell
$env:WS_VOXEL_TERRAIN_VOXELS = "C:\terrain\terrain.rgba-voxels"
```

When supplied alongside PNG and metadata, the artifact is validated field by
field but is never used as the source of the imported world.


The adapter's headless acceptance suite is `test_voxel_terrain_import`. It
covers material/biome mapping, feature metadata retention, water and air,
chunk-edge coordinates, and failure atomicity for unsupported materials,
inconsistent columns and partial density.
