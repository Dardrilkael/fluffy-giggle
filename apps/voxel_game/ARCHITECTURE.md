# Voxel game: reusable library boundaries

The app is a composition layer. Algorithms and policies that can stand alone
live in the workspace libraries; the app retains the block vocabulary,
resource pack, world rules, and rendering composition.

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
- This is a structural performance design, not a measured benchmark claim. Build, test, and FPS comparison are still required on the target machine.