%% Shadow Optimization Plan — fluffy-giggle voxel renderer
% Status: PLANNED
% Created: 2026-11-26
%
% Purpose:
%   Optimize optional directional shadows using controlled GPU benchmarks.
%   Do not change renderer behavior until the experiments identify the
%   dominant costs and an acceptable visual-quality/performance trade-off.
%
% Repository:
%   Dardrilkael/fluffy-giggle
%   Branch: feature/voxel-inventory-crafting
%
% Current measured baseline (shadow isolation, render distance 40):
%   Resolution:       1280 x 720
%   Visible chunks:   20
%   Draw calls:       82
%   Triangles:        5,694
%
%   Mode             GPU time (ms)    Average FPS
%   off                 0.724             937
%   no-receive          1.480             543
%   full                2.137             389
%
% Isolated shadow costs:
%   Shadow-map pass:   1.480 - 0.724 = 0.756 ms
%   PCF receiving:     2.137 - 1.480 = 0.657 ms
%   Total shadow cost: 2.137 - 0.724 = 1.413 ms
%
% Approximate share of added shadow cost:
%   Shadow-map pass: 53.5%
%   PCF filtering:   46.5%
%
% IMPORTANT:
%   These costs are specific to the tested GPU, driver, scene and settings.
%   The normal game configuration currently has directional shadows disabled.
%   Preserve the no-shadow baseline and keep shadows independently configurable.

clearvars;

%% Experiment A — Shadow-map resolution sweep
% Goal:
%   Determine how much of the depth-only shadow pass cost is resolution-bound.
%
% Keep constant across all runs:
%   WS_VOXEL_BENCH=23                 (3 s warm-up + 20 s measurement)
%   WS_VOXEL_BENCH_MODE=scale
%   WS_VOXEL_BENCH_VSYNC=0
%   WS_VOXEL_BENCH_WIDTH=1280
%   WS_VOXEL_BENCH_HEIGHT=720
%   WS_VOXEL_BENCH_DISTANCE=40
%   WS_VOXEL_BENCH_SHADOW_MODE=no-receive
%
% Change only the shadow-map resolution.
% Add a runtime setting/environment variable for this if one does not exist.
% Do not hard-code a temporary resolution into the renderer for each run.

experimentA.resolutions = [2048, 1024, 512, 256];
experimentA.shadowMode = "no-receive";
experimentA.runSeconds = 23;
experimentA.warmupSeconds = 3;
experimentA.measureSeconds = 20;
experimentA.renderWidth = 1280;
experimentA.renderHeight = 720;
experimentA.renderDistance = 40;

% Record for every run:
%   - Actual shadow-map dimensions (verify in benchmark configuration output)
%   - GPU frame time: mean and available percentiles
%   - CPU update and draw-submission time
%   - Wall-frame time and average FPS
%   - Visible chunks, draw calls, triangles, mesh jobs and upload bytes
%   - Any invalid/missing GPU timestamp samples
%   - Visual inspection of shadow-map quality (use a separate full-shadow run)
%
% Interpretation:
%   If reducing resolution substantially reduces GPU time, shadow-map
%   rasterization/fill or resolution-dependent work is important.
%   If GPU time barely changes, investigate pass overhead, depth rendering,
%   scene submission, synchronization and other resolution-independent costs.
%   Look for the point where further resolution reduction gives diminishing
%   returns or unacceptable shadow quality.

%% Experiment B — PCF filtering sweep
% Run only after selecting a resolution from Experiment A.
%
% Keep the selected shadow-map resolution and all scene/benchmark settings
% fixed. Use full-shadow mode for these tests.
%
% Compare:
%   B1: Existing 3x3 PCF (baseline)
%   B2: 2x2 PCF
%   B3: Single-tap depth comparison
%   B4: Optimized 3x3 PCF (reduce redundant calculations/texture operations
%       where possible without changing the intended filter footprint)
%
% For each variant record GPU time, CPU time, wall-frame time and visual quality.
% Inspect aliasing, acne, peter-panning, edge stability and shadow softness.
% A faster filter is not acceptable if it introduces severe visual artifacts.

experimentB.variants = ["3x3-current", "2x2", "1-tap", "3x3-optimized"];
experimentB.shadowMode = "full";

%% Decision criteria
% 1. Compare only runs with matching scene counters and valid GPU timings.
% 2. Repeat suspicious/outlier runs; do not select a winner from one noisy run.
% 3. Compare GPU time first, then wall-frame time and CPU overhead.
% 4. Record visual quality alongside performance.
% 5. Keep shadow rendering optional and preserve the current no-shadow path.
% 6. Do not combine resolution and PCF changes in the same experiment.
% 7. After choosing changes, rerun off / no-receive / full to verify the
%    final implementation and quantify the total improvement.

%% Planned implementation order
% [ ] Inspect current shadow-map creation and renderer configuration.
% [ ] Add a runtime-configurable shadow-map resolution (default unchanged).
% [ ] Print the actual shadow-map dimensions in benchmark configuration.
% [ ] Build Release and run the existing voxel logic tests.
% [ ] Execute Experiment A and save all raw CSV/log files.
% [ ] Analyze resolution scaling and choose a quality/performance point.
% [ ] Execute Experiment B and save all raw CSV/log files.
% [ ] Implement only the measured winning optimizations.
% [ ] Re-run regression tests and all three shadow modes.
% [ ] Update VOXEL_ENGINE_ROADMAP.md with results and decisions.

%% Output organization suggestion
% benchmark-results/shadows/resolution/
%   shadow_2048_no_receive.csv
%   shadow_1024_no_receive.csv
%   shadow_512_no_receive.csv
%   shadow_256_no_receive.csv
%   run-notes.md
%
% benchmark-results/shadows/pcf/
%   pcf_3x3_current.csv
%   pcf_2x2.csv
%   pcf_1tap.csv
%   pcf_3x3_optimized.csv
%   run-notes.md

disp("Shadow optimization plan loaded. Experiments A and B are not yet run.");
