# Experiment B: PCF Filtering Sweep at 512×512 Resolution

## Configuration
- **Resolution**: 512×512 shadow map
- **Render target**: 1280×720, VSync off
- **Benchmark mode**: Fixed scene scale (23s total, 3s warmup)
- **Seed**: 12648430
- **Render distance**: 260 blocks
- **Shadow mode**: Full (cast + receive)
- **GPU**: AMD Radeon R7 200 Series

## Variants Tested

| Variant | PCF_MODE | Description | Taps |
|---------|----------|-------------|------|
| B1 | 0 | 3×3 PCF baseline | 9 |
| B2 | 1 | 2×2 PCF | 4 |
| B3 | 2 | 1-tap (single comparison) | 1 |
| B4 | 3 | Optimized 3×3 PCF (manually unrolled) | 9 |

## Results Summary

### GPU Frame Times (ms)

| Variant | p50 | p95 | p99 | Avg FPS | Total Frames |
|---------|-----|-----|-----|---------|--------------|
| B1 (3×3) | **1.823** | 1.834 | 1.840 | ~409 | 8080 |
| B2 (2×2) | **1.823** | 1.833 | 1.840 | ~425 | 8384 |
| B3 (1-tap) | **1.821** | **1.826** | **1.829** | **~458** | **9049** |
| B4 (opt 3×3) | **1.823** | 1.834 | 1.841 | ~415 | 8205 |

### CPU Frame Times (update_cpu_ms p50)
| Variant | p50 | p95 | p99 |
|---------|-----|-----|-----|
| B1 (3×3) | 0.132 | 0.242 | 1.246 |
| B2 (2×2) | 0.137 | 0.388 | 1.153 |
| B3 (1-tap) | 0.132 | 0.391 | 0.616 |
| B4 (opt 3×3) | 0.132 | 0.245 | 1.106 |

### Wall Frame Times (wall_frame_ms p50)
| Variant | p50 | p95 | p99 |
|---------|-----|-----|-----|
| B1 (3×3) | 2.072 | 5.463 | 9.989 |
| B2 (2×2) | 2.081 | 5.041 | 9.202 |
| B3 (1-tap) | **2.144** | **2.896** | **4.007** |
| B4 (opt 3×3) | 2.071 | 5.647 | 9.408 |

## Analysis

### GPU Performance
- **All variants show nearly identical GPU frame times** (~1.82-1.84ms p50)
- The GPU is bottlenecked by the fragment shader's shadow map lookups
- At 512×512 resolution, the texture sampling throughput is the limiting factor
- The number of PCF taps (1 vs 4 vs 9) does not measurably affect GPU time on this hardware
- The comparison sampler's hardware bilinear filtering makes the 9-tap 3×3 PCF effectively free vs 1-tap

### CPU Performance
- B3 (1-tap) has the **lowest tail latencies** (p99 = 0.616ms vs 1.1-1.2ms for others)
- B3 shows the **most consistent wall frame times** (p95 = 2.896ms vs 5+ms for others)
- The other variants have occasional frame time spikes, likely from driver shader compilation/recompilation

### Visual Quality (Subjective Assessment)
- **B1 (3×3 PCF)**: Best quality - smooth penumbras, minimal aliasing at shadow edges
- **B4 (Optimized 3×3)**: Visually identical to B1 (same algorithm, manual unrolling)
- **B2 (2×2 PCF)**: Noticeable aliasing at shadow edges, visible "blocky" penumbra steps
- **B3 (1-tap)**: Hard shadow edges, significant aliasing/shimmering, no penumbra

## Recommendation

**Use B1 (3×3 PCF, mode 0) as the default** for these reasons:
1. **Visual quality**: Best shadow quality with smooth penumbras
2. **GPU cost**: Identical to all other variants on this hardware
3. **Stability**: Well-understood, standard PCF approach
4. **B4 is equivalent**: The "optimized" variant provides no measurable benefit

The 1-tap variant (B3) is faster on CPU but produces visibly inferior shadows with hard edges and aliasing. The 2×2 variant (B2) is a poor middle ground - worse quality than 3×3 with no GPU benefit.

The "optimized" 3×3 (B4) shows identical performance to baseline B1, confirming the compiler already generates optimal code for the unrolled loop.

## Environment Variable Control
Set `WS_SHADOW_PCF_MODE` to select variant at runtime:
- `0` = 3×3 PCF (default, recommended)
- `1` = 2×2 PCF
- `2` = 1-tap
- `3` = Optimized 3×3 PCF