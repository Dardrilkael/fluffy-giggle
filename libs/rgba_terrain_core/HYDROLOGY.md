# Deterministic hydrology (bounded drainage pass)

Hydrology is evaluated from absolute world coordinates and the world plan. It
does not allocate a world-sized drainage raster or depend on chunk generation
order.

## Drainage and accumulation

The routing grid uses the configured absolute cell spacing
(`WorldHydrologySettings::routing_cell_scale`). For each query, the sampler
builds a fixed 11x11 routing window plus a one-cell D8 elevation halo. Every
cell selects its steepest strictly downhill D8 neighbor; local minima are
terminal sinks.

Runoff is initialized per routing cell from the plan's moisture setting and a
deterministic spatial field. The cells are then processed from high elevation
to low elevation, propagating their accumulated runoff along the D8 graph.
This produces connected local stream networks and avoids retracing each source
path independently. `WorldSample::watershed_accumulation` is the accumulated
contributing runoff inside this bounded window, not a globally complete
catchment value.

The window is fixed and centered on the queried routing cell in absolute grid
coordinates. Its result is therefore deterministic for a fixed plan and
coordinate, independent of chunk size and generation order. Contributions
that leave the window are intentionally not represented; this remains a
bounded local approximation rather than a global drainage graph.

## Lakes and spill levels

The sampler follows the queried cell's D8 path to its terminal sink. A
minimax flood within the same local window finds the lowest escape elevation
from that sink to the window boundary. Depression depth, accumulated runoff
and a deterministic moisture estimate determine whether the sink forms a lake.

Cells that drain into a qualifying sink and remain below its spill elevation
are classified as part of that lake basin. All such columns encode the same
spill elevation, producing a flat reconstructed water surface. The current
RGBA format has only one elevation per column, so the underwater basin floor
is not separately retained beneath lake water.

## Rivers

Cells above the configured contributing-runoff threshold become river
candidates unless they are inside a lake basin or are terminal sinks. River
strength controls a gradual channel-depth cut in the encoded height field.
River columns reconstruct water immediately above the channel bed.

## RGBA and voxel compatibility

The four-channel PNG layout remains unchanged. The plan-based generator writes v2: R/G are 16-bit elevation, B stores a four-bit material plus four feature flags, and A stores the full biome byte. Legacy v1 maps remain readable.

Lake columns encode their shared spill elevation; river columns encode their
carved channel bed. Both reconstruct one water voxel above the encoded surface.
Ocean columns retain the existing sea-level fill behavior. No climate or
hydrology diagnostics are packed into new channels.

## Limitations and next refinements

The routing window bounds the contributing area and spill search. A later
regional drainage graph can remove this local-window limit while retaining
absolute-coordinate determinism. The model does not yet simulate infiltration,
soil storage, evaporation, seasonality, sediment transport or erosion.
