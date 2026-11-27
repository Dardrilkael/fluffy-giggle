# Coupled environmental model (initial pass)

The world sampler now evaluates a deterministic environmental chain at each global
coordinate. It remains a coordinate sampler rather than a time-stepping simulation,
so a chunk can be generated independently without loading neighboring chunks or
depending on generation order.

## Current dependency chain

1. Continental and regional terrain determine normalized elevation and mountain
   strength.
2. Latitude, regional temperature influence and elevation determine temperature.
   Elevation cooling uses the configured lapse rate above sea level.
3. A short fetch along the configured prevailing-wind direction samples upstream
   terrain. Ocean samples contribute moisture availability; rising terrain creates
   orographic lift; higher upstream ridges create a leeward rain-shadow effect.
4. Warmer sea-level air increases the ocean-vapor contribution. Orographic ascent
   cools incoming air and increases condensation efficiency.
5. Precipitation consumes a small portion of available moisture. Ocean samples
   retain a humidity floor to represent continuous surface moisture supply.
6. Temperature, remaining moisture availability and elevation select ecosystem
   and biome suitability.

## Diagnostics

WorldSample exposes orographic_lift, rain_shadow and ocean_moisture in addition
to temperature, humidity and precipitation. These values are normalized indices
intended for inspection, future tuning and AI-plan feedback.

humidity currently means normalized moisture availability, not calibrated
relative humidity. precipitation means normalized precipitation potential, not
a rainfall depth or rate. The model is a coherent procedural climate heuristic,
not a numerical atmospheric solver.

## Determinism and region generation

Every terrain fetch uses the same seed, plan and absolute coordinates. The
atmospheric fetch is allowed to inspect coordinates beyond the finite map edge:
this models air entering the map from outside and avoids artificial climate
boundaries at world edges. Public sample() and RGBA region requests still reject
coordinates outside the finite world.

The existing RGBA PNG layout is unchanged:
- R/G: 16-bit normalized elevation.
- B: material ID.
- A: biome ID and existing feature/water flags.

Climate diagnostics are not squeezed into the PNG channels. They remain available
through WorldSample; later work can add a separate versioned climate-map format
or recompute them from the world plan. The PNG remains the editable terrain map
and the existing voxel reconstruction pipeline remains authoritative.
