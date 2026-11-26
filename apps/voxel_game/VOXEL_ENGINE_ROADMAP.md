# Voxel Engine Roadmap

Use Luanti as an architectural reference, not as a codebase to copy. The goal
is a compact, data-oriented voxel engine optimized for cache locality, bounded
memory, low frame-time variance and static dispatch.

## Design rules

- Compact block IDs in contiguous chunk storage; direct indexing in hot loops.
- No per-voxel allocations, pointer-heavy block objects or virtual block calls.
- World data and CPU meshing do not own GPU handles or graphics lifetime.
- Keep content definitions separate from generic algorithms.
- Background queues are bounded, coalesced and revision-aware.
- Generation remains deterministic regardless of chunk/job order.
- Measure before changing chunk layout, draw/material grouping or compression.

## Target ownership boundaries

| Module | Responsibility |
|---|---|
| `ws::world` (future) | Chunk storage, coordinates, revisions, dirty state, load/evict lifecycle, CPU mesh data |
| `ws::world::generation` (future) | Deterministic terrain, biome, cave, ore and decoration passes |
| `voxel` application | Block/item IDs, material definitions, rules, recipes and game content |
| `voxel::VoxelRenderer` | GPU chunk meshes, uploads, draw ordering and GPU lifetime |
| `ws::physics3d` | Voxel collision and character movement |
| `ws::gameplay` | Inventory, crafting and tool capability evaluation |
| `ws::audio` | Audio DSP and playback |

The generic chunk-storage template currently lives in `ws::gameplay`; move it
only as a deliberate API migration, never by duplicating the implementation.

## Milestones

### 0 — correctness and measurement

- [x] Preserve deterministic world-generation and controller tests.
- [x] Keep `WS_VOXEL_BENCH` as the repeatable CPU benchmark.
- [x] Measure world generation, CPU meshing and CPU upload staging/submission separately; report payload bytes and generated vertex/index counts.
- [x] Report rolling frame-time p50/p95/p99 and per-benchmark-phase percentiles.
- [x] Add per-frame-slot Vulkan timestamp queries around the submitted shadow + main render passes; collect only after the slot fence, without stalling or including presentation/vsync.
- [x] Add a fixed-camera scalability benchmark with a warm-up interval, configurable render distance/window extent/VSync, per-second scene counters, and update-CPU, wall-frame and GPU p50/p95/p99.
- [x] Keep benchmark update cost separate from the wall interval between update starts so presentation/pacing does not get mislabeled as game CPU work.
- [x] Add benchmark-only shadow modes to distinguish the receiver's 3×3 PCF filtering cost from the depth-only shadow-map pass.

**Scalability benchmark protocol**

Set `WS_VOXEL_BENCH_MODE=scale`, `WS_VOXEL_BENCH=23` (3 seconds warm-up + 20 seconds measured), `WS_VOXEL_BENCH_DISTANCE`, `WS_VOXEL_BENCH_WIDTH`, `WS_VOXEL_BENCH_HEIGHT`, and `WS_VOXEL_BENCH_VSYNC=0`. The fixed mode does not move the player or edit blocks. Keep the seed and camera pose unchanged while sweeping one variable at a time. Capture the emitted `bench:config` line, one-second CSV rows and final percentiles for each run.

To isolate shadow cost, repeat one identical fixed-scene configuration with `WS_VOXEL_BENCH_SHADOW_MODE=full`, `no-receive`, and `off`. `no-receive` still renders the depth-only shadow map but disables receiver PCF lookups; `off` disables both. The full-to-no-receive GPU-time delta estimates PCF cost, while no-receive-to-off estimates shadow-map rendering cost. Unsupported queues report GPU timing unavailable.

### 1 — CPU meshing and renderer ownership

- [x] Remove GPU handles and upload/destroy calls from `World`.
- [x] Introduce renderer-independent CPU mesh data.
- [x] Move GPU chunk ownership to `VoxelRenderer`.
- [x] Replace per-face `vector<bool>` allocations with fixed scratch arrays
  (maximum plane = 16×64 = 1024 cells).
- [x] Read interior cells through one chunk span; use global lookup only for
  the one-cell boundary halo.
- [x] Add a headless CPU mesh validity test.
- [ ] Verify CPU/GPU suites on CI and target PC.
- [ ] Compare meshing time and edit-time frame spikes against baseline.
- [x] Reuse CPU mesh output capacity across rebuilds, including async result handoff through buffer swaps.
- [x] Classify all exposed faces and tile IDs in one chunk traversal, then
  build greedy masks by tile without rereading voxel neighbors.
- [x] Cache chunk bounds/frustum visibility once per chunk per frame.
- [x] Keep tile-indexed CPU mesh vectors in the renderer and clear them
  without releasing capacity between rebuilds.
- [x] Replace the per-rebuild heap face cache with a fixed-size cache.
- [x] Reuse renderer upload-staging vectors and count renderer vector-capacity growths. The renderer API still copies into its own CPU/GPU mesh storage; those internal allocations are not included in this counter.
- [ ] Verify CPU/GPU suites on CI and target PC.

**Exit:** same world geometry, CPU meshing without an active renderer, and no
regression in edit-time p95 frame time.

### 2 — bounded asynchronous meshing

- [x] Fixed-capacity dirty-chunk queue with duplicate coalescing.
- [x] Monotonically increasing per-chunk revisions.
- [x] Immutable chunk-plus-halo snapshots before worker dispatch.
- [x] Worker thread meshes snapshots only; no renderer/GPU access.
- [x] Discard results whose source revision is stale.
- [x] Render-thread upload with a two-chunk-per-frame job budget.
- [x] Prioritize queued chunks by current player X/Z distance at worker dequeue time; initial generation remains synchronous.
- [x] Join worker before releasing snapshots during shutdown.
- [x] Extract the bounded worker queue into a CPU-only AsyncChunkMesher;
  the renderer now only consumes completed CPU results and owns GPU uploads.
- [x] Cover rapid same-chunk edits, newest-revision publication, coalescing,
  result invalidation and worker shutdown in the normal headless logic suite.

**Exit:** gameplay edits never synchronously mesh/upload; stale jobs cannot
overwrite current geometry; queue memory is bounded.

### 3 — block definitions and tool capabilities

- [ ] Compact `BlockDef` and `ToolDef` tables indexed by IDs.
- [ ] Encode solidity, render class, occlusion, collision, mining group/level,
  hardness, drop, sound and emitted light.
- [ ] Pure `get_dig_result(block, tool, wear)` calculation.
- [ ] Held-button mining progress; cancel when target changes.
- [ ] Crack-stage overlay, mining levels and material-specific speeds.
- [ ] Exact durability/wear behavior and hand-mining fallback.
- [ ] Tests for tool selection, level requirements, hardness, wear edge cases.

No per-block heap objects, virtual dispatch, string comparisons or registry
lookups in the mining loop.

### 4 — staged procedural generation

- [ ] Split generation into terrain/climate, biome surface, cave, ore,
  decoration and fluid-fill passes.
- [ ] Keep parameters/content rules separate from pass algorithms.
- [ ] Biomes use climate and optional altitude/vertical blending.
- [ ] Ore rules encode host blocks, depth, scarcity, shape and seeded offsets.
- [ ] Decoration rules encode place-on, neighbor constraints and density.
- [ ] Structures can cross chunk boundaries without seams.
- [ ] Reuse column climate/height fields instead of sampling repeatedly.
- [ ] Verify chunk-order-independent generation.

### 5 — chunk lifecycle and streaming

- [ ] Move generic chunk storage from `ws::gameplay` to `ws::world`.
- [ ] Separate world/chunk coordinates from fixed world dimensions.
- [ ] Sparse directory of loaded chunks; dense contiguous storage per chunk.
- [ ] Explicit states: absent, requested, generating, resident, dirty,
  meshing, ready-to-upload, saving and evictable.
- [ ] Add vertical chunking only when world-height/streaming requirements justify it.
- [ ] Benchmark 16³, 16×32×16 and current 16×64×16 layouts.
- [ ] Bound generation and unload work per frame.

Do not blindly copy Luanti's 16³ MapBlock. The current 16×64×16 chunk has
16,384 cells. A 16³ chunk has 4,096 cells and would produce four times as
many chunk records for this world's current 64-block height. Smaller jobs
improve granularity but increase bookkeeping and may increase draw calls.
Choose by measurement.

### 6 — persistence

- [ ] Versioned chunk header with coordinates, generator version, palette/
  payload lengths and checksum.
- [ ] Serialize compact block IDs; palette compression where profitable.
- [ ] Evaluate RLE only where it beats the simpler palette format.
- [ ] Save through a temporary file and atomic replace.
- [ ] Bounded asynchronous disk I/O with dirty-revision tracking.
- [ ] Recover from truncated, corrupt or unknown-version chunks.
- [ ] Never serialize GPU meshes; rebuild from voxel data.

### 7 — lighting and fluids

- [ ] Compact sunlight and local-light values per cell.
- [ ] Propagate light through active-cell queues, not world scans each frame.
- [ ] Update only regions affected by block/light-source edits.
- [ ] Deterministic gravity/lateral fluid spread with source/flowing states.
- [ ] Bound work per tick and carry unfinished work across frames.
- [ ] Test propagation and source removal across chunk borders.

### 8 — world interactions and content

- [ ] Sparse metadata for interactive blocks, not objects per voxel.
- [ ] Containers, furnaces/workstations and dropped items.
- [ ] Validated palette-based structure/schematic placement.
- [ ] Content registration only if modding is actually required; retain static
  dispatch in hot simulation paths.

## Performance acceptance metrics

| Metric | Purpose |
|---|---|
| Generation ms/chunk | World creation cost |
| CPU meshing ms/chunk | Mesher throughput |
| Exposed faces vs emitted quads | Greedy merging effectiveness |
| Upload bytes/ms | Render-thread transfer pressure |
| Dirty, queued and stale chunks | Async queue health |
| Visible chunks and draw calls | Culling/submission efficiency |
| Frame time p50/p95/p99 | Smoothness and spikes |
| GPU frame time | Actual submitted graphics work; Vulkan timestamp interval, excluding CPU recording and presentation |
| Resident bytes/chunk | Memory efficiency |

Every performance change needs before/after measurements using the same seed,
camera path, viewport and build configuration. Fewer lines or generic
interfaces are not proof of higher performance.

## Luanti references

- [Biome generation](https://github.com/luanti-org/luanti/blob/master/src/mapgen/mg_biome.cpp)
- [Ore generation](https://github.com/luanti-org/luanti/blob/master/src/mapgen/mg_ore.cpp)
- [Decoration placement](https://github.com/luanti-org/luanti/blob/master/src/mapgen/mg_decoration.cpp)
- [Tool capabilities and wear](https://github.com/luanti-org/luanti/blob/master/src/tool.cpp)
- [Client mesh worker](https://github.com/luanti-org/luanti/blob/master/src/client/mesh_generator_thread.cpp)
