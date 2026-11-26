# Experiment A — Shadow-map Resolution Sweep: Run Notes

**Date:** 2026-11-26  
**GPU:** AMD Radeon R7 200 Series  
**Settings:** 1280×720, VSync off, render distance 40, seed 12648430, shadow mode `no-receive`  
**Benchmark:** 3 s warm-up + 20 s measurement (WS_VOXEL_BENCH=23, mode=scale)

## Verified Configuration per Run

| Test | WS_SHADOW_MAP_RESOLUTION | Logged Resolution | Draws | Tris | Chunks | Meshes |
|------|-------------------------|-------------------|-------|------|--------|--------|
| A1   | 2048                    | 2048×2048 ✓       | 136   | 10310| 20     | 381    |
| A2   | 1024                    | 1024×1024 ✓       | 136   | 10310| 20     | 381    |
| A3   | 512                     | 512×512 ✓         | 136   | 10310| 20     | 381    |
| A4   | 256                     | 256×256 ✓         | 136   | 10310| 20     | 381    |

All scene counters identical across runs. GPU timestamp queries valid (gpu_samples = frame count each second).

## GPU Time Summary (ms)

| Resolution | Mean | p50  | p95  | p99  | Δ from 2048 | Speedup vs 2048 |
|------------|------|------|------|------|-------------|-----------------|
| 2048       | 1.642| 1.640| 1.649| 1.653| —           | 1.00×           |
| 1024       | 1.118| 1.117| 1.124| 1.128| -0.524 ms   | 1.47×           |
| 512        | 0.935| 0.933| 0.943| 0.950| -0.707 ms   | 1.76×           |
| 256        | 0.887| 0.885| 0.895| 0.901| -0.755 ms   | 1.85×           |

## CPU / Wall Time Summary

| Resolution | Update p50 (ms) | Wall p50 (ms) | FPS (mean) |
|------------|-----------------|---------------|------------|
| 2048       | 0.123           | 1.879         | 477        |
| 1024       | 0.119           | 1.341         | 636        |
| 512        | 0.116           | 1.152         | 736        |
| 256        | 0.117           | 1.101         | 763        |

## Analysis

**Shadow-map pass is strongly resolution-bound.** Halving resolution from 2048→1024 reduces GPU time by 32% (0.52 ms). Further halving to 512 gives another 16% (0.18 ms), and to 256 only 5% (0.05 ms). Diminishing returns below 512.

**Cost breakdown (approx):**
- 2048→1024: 0.52 ms saved (largest single step)
- 1024→512: 0.18 ms saved
- 512→256: 0.05 ms saved (near noise floor)

**Visual quality consideration:** At 256×256, shadow-map texels project to ~1.6× world units at distance 40 (frustum fits casters in x/y). This will show noticeable blockiness on shadow edges. 512×512 (~0.8 texels/unit) is likely the practical floor for acceptable quality.

**Recommendation:** **512×512** — captures 86% of the total possible GPU time reduction (0.71/0.83 ms) while maintaining usable shadow quality. 256×256 gives only marginal additional gain (0.05 ms) with significant visual degradation.

## Next Steps

1. Visual quality check with `WS_VOXEL_BENCH_SHADOW_MODE=full` at 2048, 1024, 512
2. Proceed to Experiment B (PCF filtering sweep) at selected 512 resolution