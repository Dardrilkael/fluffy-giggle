# Shadow Optimization Plan — fluffy-giggle voxel renderer

**Status:** Planned  
**Created:** 2026-11-26  
**Repository:** [Dardrilkael/fluffy-giggle](https://github.com/Dardrilkael/fluffy-giggle)  
**Branch:** `feature/voxel-inventory-crafting`

## Purpose

Optimize optional directional shadows using controlled GPU benchmarks. Do not change renderer behavior until the experiments identify the dominant costs and an acceptable visual-quality/performance trade-off.

## Current measured baseline

Shadow-isolation benchmark at render distance 40, 1280×720:

| Mode | GPU time | Average FPS |
|---|---:|---:|
| `off` | 0.724 ms | 937 |
| `no-receive` | 1.480 ms | 543 |
| `full` | 2.137 ms | 389 |

Scene counters: 20 visible chunks, 82 draw calls, 5,694 triangles.

### Isolated shadow costs

| Component | Calculation | Cost |
|---|---|---:|
| Shadow-map pass | 1.480 − 0.724 | **0.756 ms** |
| PCF receiving | 2.137 − 1.480 | **0.657 ms** |
| Total shadow cost | 2.137 − 0.724 | **1.413 ms** |

Approximate share of added shadow cost:

- Shadow-map pass: **53.5%**
- PCF filtering: **46.5%**

> These costs are specific to the tested GPU, driver, scene and settings. The normal game configuration currently has directional shadows disabled. Preserve the no-shadow baseline and keep shadows independently configurable.

## Experiment A — Shadow-map resolution sweep

**Goal:** Determine how much of the depth-only shadow-pass cost is resolution-bound.

Keep constant across all runs:

| Setting | Value |
|---|---|
| `WS_VOXEL_BENCH` | `23` (3 s warm-up + 20 s measurement) |
| `WS_VOXEL_BENCH_MODE` | `scale` |
| `WS_VOXEL_BENCH_VSYNC` | `0` |
| `WS_VOXEL_BENCH_WIDTH` | `1280` |
| `WS_VOXEL_BENCH_HEIGHT` | `720` |
| `WS_VOXEL_BENCH_DISTANCE` | `40` |
| `WS_VOXEL_BENCH_SHADOW_MODE` | `no-receive` |

Change only the shadow-map resolution:

| Test | Shadow-map resolution |
|---|---:|
| A1 | 2048×2048 |
| A2 | 1024×1024 |
| A3 | 512×512 |
| A4 | 256×256 |

Add a runtime setting/environment variable for resolution if one does not exist. Do not hard-code a temporary resolution into the renderer for each run.

### Record for every run

- Actual shadow-map dimensions (verify in benchmark configuration output).
- GPU frame time: mean and available percentiles.
- CPU update and draw-submission time.
- Wall-frame time and average FPS.
- Visible chunks, draw calls, triangles, mesh jobs and upload bytes.
- Invalid or missing GPU timestamp samples.
- Visual inspection of shadow-map quality (use a separate full-shadow run).

### Interpretation

- If reducing resolution substantially reduces GPU time, shadow-map rasterization/fill or resolution-dependent work is important.
- If GPU time barely changes, investigate pass overhead, depth rendering, scene submission, synchronization and other resolution-independent costs.
- Identify where further resolution reduction gives diminishing returns or unacceptable shadow quality.

## Experiment B — PCF filtering sweep

Run only after selecting a resolution from Experiment A.

Keep the selected shadow-map resolution and all scene/benchmark settings fixed. Use `full` shadow mode.

| Test | Filtering |
|---|---|
| B1 | Existing 3×3 PCF (baseline) |
| B2 | 2×2 PCF |
| B3 | Single-tap depth comparison |
| B4 | Optimized 3×3 PCF, reducing redundant calculations/texture operations where possible without changing the intended filter footprint |

For each variant, record GPU time, CPU time, wall-frame time and visual quality. Inspect aliasing, shadow acne, peter-panning, edge stability and shadow softness.

A faster filter is not acceptable if it introduces severe visual artifacts.

## Decision criteria

1. Compare only runs with matching scene counters and valid GPU timings.
2. Repeat suspicious/outlier runs; do not select a winner from one noisy run.
3. Compare GPU time first, then wall-frame time and CPU overhead.
4. Record visual quality alongside performance.
5. Keep shadow rendering optional and preserve the current no-shadow path.
6. Do not combine resolution and PCF changes in the same experiment.
7. After choosing changes, rerun `off` / `no-receive` / `full` to verify the final implementation and quantify the total improvement.

## Planned implementation order

- [ ] Inspect current shadow-map creation and renderer configuration.
- [ ] Add a runtime-configurable shadow-map resolution (default unchanged).
- [ ] Print the actual shadow-map dimensions in benchmark configuration.
- [ ] Build Release and run the existing voxel logic tests.
- [ ] Execute Experiment A and save all raw CSV/log files.
- [ ] Analyze resolution scaling and choose a quality/performance point.
- [ ] Execute Experiment B and save all raw CSV/log files.
- [ ] Implement only the measured winning optimizations.
- [ ] Rerun regression tests and all three shadow modes.
- [ ] Update `VOXEL_ENGINE_ROADMAP.md` with results and decisions.

## Suggested output organization

```text
benchmark-results/shadows/
├── resolution/
│   ├── shadow_2048_no_receive.csv
│   ├── shadow_1024_no_receive.csv
│   ├── shadow_512_no_receive.csv
│   ├── shadow_256_no_receive.csv
│   └── run-notes.md
└── pcf/
    ├── pcf_3x3_current.csv
    ├── pcf_2x2.csv
    ├── pcf_1tap.csv
    ├── pcf_3x3_optimized.csv
    └── run-notes.md
```

**Current state:** planning only. Experiments A and B have not yet been run.
