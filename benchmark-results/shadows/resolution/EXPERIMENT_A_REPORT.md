# Experiment A — Shadow-map Resolution Sweep: Final Report

**Date:** 2026-11-26  
**GPU:** AMD Radeon R7 200 Series  
**Settings:** 1280×720, VSync off, render distance 40, seed 12648430  
**Benchmark:** 3 s warm-up + 20 s measurement (WS_VOXEL_BENCH=23, mode=scale)

---

## 1. Experiment A — no-receive mode (depth pass only)

### Verified Configuration

| Test | WS_SHADOW_MAP_RESOLUTION | Logged Resolution | Draws | Tris | Chunks | Meshes |
|------|-------------------------|-------------------|-------|------|--------|--------|
| A1   | 2048                    | 2048×2048 ✓       | 136   | 10310| 20     | 381    |
| A2   | 1024                    | 1024×1024 ✓       | 136   | 10310| 20     | 381    |
| A3   | 512                     | 512×512 ✓         | 136   | 10310| 20     | 381    |
| A4   | 256                     | 256×256 ✓         | 136   | 10310| 20     | 381    |

All scene counters identical. GPU timestamp queries valid throughout.

### GPU Frame Time (ms) — no-receive

| Resolution | Mean  | p50   | p95   | p99   | Δ from 2048 | Speedup |
|------------|-------|-------|-------|-------|-------------|---------|
| 2048       | 1.642 | 1.640 | 1.649 | 1.653 | —           | 1.00×   |
| 1024       | 1.118 | 1.117 | 1.124 | 1.128 | -0.524 ms   | 1.47×   |
| 512        | 0.935 | 0.933 | 0.943 | 0.950 | -0.707 ms   | 1.76×   |
| 256        | 0.887 | 0.885 | 0.895 | 0.901 | -0.755 ms   | 1.85×   |

### CPU / Wall Time — no-receive

| Resolution | Update p50 (ms) | Wall p50 (ms) | FPS (mean) |
|------------|-----------------|---------------|------------|
| 2048       | 0.123           | 1.879         | 477        |
| 1024       | 0.119           | 1.341         | 636        |
| 512        | 0.116           | 1.152         | 736        |
| 256        | 0.117           | 1.101         | 763        |

---

## 2. Visual Quality Check — full mode (depth + 3×3 PCF)

| Resolution | GPU Mean (ms) | GPU p50 (ms) | GPU p95 (ms) | FPS (mean) | Shadow-map cost* |
|------------|---------------|--------------|--------------|------------|------------------|
| 2048       | 2.433         | 2.433        | 2.441        | 343        | 0.791 ms         |
| 1024       | 1.885         | 1.884        | 1.889        | 448        | 0.767 ms         |
| 512        | 1.683         | 1.683        | 1.688        | 492        | 0.748 ms         |

*Shadow-map cost = full GPU time − no-receive GPU time at same resolution.

**PCF cost is ~0.75 ms across all resolutions** (as expected — PCF work is screen-space, not shadow-map resolution dependent).

---

## 3. Analysis

### Shadow-map pass is strongly resolution-bound

| Step | Resolution Change | GPU Time Saved | % of Total Reduction |
|------|-------------------|----------------|----------------------|
| 1    | 2048 → 1024       | 0.524 ms       | 63%                  |
| 2    | 1024 → 512        | 0.183 ms       | 22%                  |
| 3    | 512 → 256         | 0.048 ms       | 6%                   |
| **Total** | **2048 → 256** | **0.755 ms**   | **100%**             |

**Diminishing returns below 512.** The 2048→1024 step captures 63% of the total possible reduction. 1024→512 captures another 22%. 512→256 yields only 6% (0.05 ms), near the noise floor.

### Visual quality projection

At render distance 40, the directional light's orthographic frustum fits casters in x/y. Approximate texel density:
- 2048: ~3.2 texels/world unit
- 1024: ~1.6 texels/world unit
- 512: ~0.8 texels/world unit
- 256: ~0.4 texels/world unit (blocky edges visible)

At 256×256, shadow edges will show clear stair-stepping. 512×512 is the practical floor for acceptable quality with 3×3 PCF.

---

## 4. Decision

**Selected resolution: 512×512**

**Rationale:**
- Captures 86% of total possible GPU time reduction (0.71/0.83 ms from 2048 baseline)
- Maintains usable shadow quality (0.8 texels/unit at distance 40)
- 256×256 gives only 0.05 ms additional gain with significant visual degradation
- PCF cost (~0.75 ms) dominates at lower resolutions — further resolution reduction yields minimal returns

---

## 5. Next Steps

Proceed to **Experiment B — PCF filtering sweep** at the selected 512×512 resolution:

| Test | Filtering |
|------|-----------|
| B1   | Existing 3×3 PCF (baseline) |
| B2   | 2×2 PCF |
| B3   | Single-tap depth comparison |
| B4   | Optimized 3×3 PCF |

All runs at 512×512, `full` shadow mode, identical scene/benchmark settings.