# RGBA terrain data-map formats

R/G remain the 16-bit elevation field in every version.

## v1 (legacy)
B is the material byte. A stores a three-bit biome ID and five feature/water
bits. Existing v1 metadata remains supported.

## v2 (plan-based generator)
B bits 0..3 store one of 15 surface materials. B bits 4..7 store forest,
mountain, river and lake flags. A stores the full 8-bit biome ID. The default
world plan defines 21 biome types and 15 surface materials.

Logical feature flags restored into voxel metadata remain 0x08, 0x10, 0x20
and 0x40. Ocean water is inferred from biome ID 0; inland water is inferred
from river/lake flags. No extra image or spatial resampling is used.
