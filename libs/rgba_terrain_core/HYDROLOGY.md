# Deterministic hydrology (initial pass)

Hydrology is evaluated from absolute world coordinates and the world plan. It
does not allocate a world-sized drainage raster or depend on which chunk was
generated first.

## Drainage

The routing grid uses a configurable absolute cell spacing
(WorldHydrologySettings::routing_cell_scale). Each cell evaluates its eight
neighbors and selects the steepest downhill D8 direction. A cell with no
downhill neighbor is marked Sink.

## Watershed estimate

The sampler traces nearby source-cell flow paths toward the requested routing
cell and sums precipitation-related runoff weights. The search is deliberately
bounded, so this is a local contributing-area estimate rather than a globally
complete watershed accumulation. The diagnostic is exposed as
WorldSample::watershed_accumulation.

## Rivers and lakes

Cells above the configured accumulation threshold become river candidates,
unless they are local sinks. River strength controls a gradual channel-depth
cut in the encoded height field. Local sinks with sufficient depression depth,
runoff and precipitation become lake candidates.

The depression estimate compares the sink with a nearby spill-rim sample. It is
a lightweight local heuristic, not a priority-flood solution; large connected
basins and globally correct lake outlets remain future work.

## RGBA and voxel compatibility

The four-channel PNG layout remains unchanged:
- R/G: 16-bit channel-bed elevation.
- B: material ID.
- A bits 0..2: biome; bits 3..6: feature flags (forest, mountain, river, lake);
  bit 7: water presence.

River channels are gradually lowered in the height channel. River and lake
columns reconstruct one water voxel above the encoded bed. Ocean columns retain
the existing sea-level fill behavior. No climate or hydrology diagnostics are
packed into new channels.

## Limitations and next refinements

The current accumulation and basin detection are bounded local estimates. A
future version should build/cache deterministic regional drainage graphs,
perform priority-flood depression filling, and derive globally connected
watersheds and river networks. Infiltration, soil storage, evaporation,
seasonality, sediment transport and erosion are not modeled yet.
