#include "rgba_terrain/rgba_terrain.hpp"

#include <cassert>
#include <stdexcept>

using namespace rgba_terrain;

int main() {
    RgbaImage image{2, 1, {{255, 7, 3, 255}, {0, 9, 4, 0}}};
    ChannelLayout layout;
    layout.channels = {
        ChannelRole::Height8,
        ChannelRole::MaterialId,
        ChannelRole::BiomeId,
        ChannelRole::WaterMask,
    };

    TerrainSettings settings;
    settings.max_height = 10.0f;
    settings.volume_height = 16;
    settings.sea_level = 5;
    settings.subsurface_depth = 2;
    const auto columns = generate_columns(image, layout, settings);
    assert(columns.width == 2 && columns.depth == 1);
    assert(columns.columns.size() == 2);
    assert(columns.columns[0].height == 10);
    assert(columns.columns[0].material_id == 7);
    assert(columns.columns[0].biome_id == 3);
    assert(columns.columns[1].height == 0);
    assert(columns.columns[1].water_mask == 0);

    // First column: top material at y=10, subsurface y=8..9, deep below.
    // Second column: only its y=0 surface is solid; no water is requested.
    const auto volume = generate_voxel_volume(columns, settings);
    assert(volume.width == 2 && volume.depth == 1 && volume.height == 16);
    assert(volume.voxels.size() == 32);
    assert(volume.at(0, 10, 0).kind == VoxelKind::Solid);
    assert(volume.at(0, 10, 0).material_id == 7);
    assert(volume.at(0, 9, 0).material_id == settings.subsurface_material);
    assert(volume.at(0, 8, 0).material_id == settings.subsurface_material);
    assert(volume.at(0, 7, 0).material_id == settings.deep_material);
    assert(volume.at(0, 11, 0).kind == VoxelKind::Air);
    assert(volume.at(1, 0, 0).kind == VoxelKind::Solid);

    // Explicitly test the water-mask path on a low column.
    image.pixels[1].a = 255;
    const auto flooded = generate_voxel_volume(image, layout, settings);
    assert(flooded.at(1, 0, 0).kind == VoxelKind::Solid);
    assert(flooded.at(1, 1, 0).kind == VoxelKind::Water);
    assert(flooded.at(1, 5, 0).kind == VoxelKind::Water);
    assert(flooded.at(1, 6, 0).kind == VoxelKind::Air);

    ChannelLayout precise_layout;
    precise_layout.channels = {
        ChannelRole::Height16Msb,
        ChannelRole::Height16Lsb,
        ChannelRole::MaterialId,
        ChannelRole::Unused,
    };
    RgbaImage precise{1, 1, {{0x80, 0x00, 12, 255}}};
    settings.max_height = 15.0f;
    settings.volume_height = 16;
    const auto precise_out =
        generate_columns(precise, precise_layout, settings);
    assert(precise_out.columns[0].height == 8);
    assert(precise_out.columns[0].material_id == 12);

    bool rejected = false;
    try {
        ChannelLayout invalid;
        invalid.channels = {
            ChannelRole::Height16Msb,
            ChannelRole::MaterialId,
            ChannelRole::BiomeId,
            ChannelRole::Unused,
        };
        invalid.validate();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        TerrainColumns invalid_columns{1, 1, {{0, 0, 16, 1, 0, 0, 255}}};
        (void)generate_voxel_volume(invalid_columns, settings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
