#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/metadata.hpp>
#include <ws/rgba_terrain_core/map_analysis.hpp>
#include <ws/rgba_terrain_core/voxel_file.hpp>
#include <ws/rgba_terrain_core/voxel_validation.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <stdexcept>

using namespace ws::rgba_terrain_core;

TEST_CASE("RGBA terrain image decoding and voxel reconstruction") {
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
    CHECK(columns.width == 2 && columns.depth == 1);
    CHECK(columns.columns.size() == 2);
    CHECK(columns.columns[0].height == 10);
    CHECK(columns.columns[0].material_id == 7);
    CHECK(columns.columns[0].biome_id == 3);
    CHECK(columns.columns[1].height == 0);
    CHECK(columns.columns[1].water_mask == 0);

    // First column: top material at y=10, subsurface y=8..9, deep below.
    // Second column: only its y=0 surface is solid; no water is requested.
    const auto volume = generate_voxel_volume(columns, settings);
    CHECK(volume.width == 2 && volume.depth == 1 && volume.height == 16);
    CHECK(volume.voxels.size() == 32);
    CHECK(volume.at(0, 10, 0).kind == VoxelKind::Solid);
    CHECK(volume.at(0, 10, 0).material_id == 7);
    CHECK(volume.at(0, 9, 0).material_id == settings.subsurface_material);
    CHECK(volume.at(0, 8, 0).material_id == settings.subsurface_material);
    CHECK(volume.at(0, 7, 0).material_id == settings.deep_material);
    CHECK(volume.at(0, 11, 0).kind == VoxelKind::Air);
    CHECK(volume.at(1, 0, 0).kind == VoxelKind::Solid);

    // Explicitly test the water-mask path on a low column.
    image.pixels[1].a = 255;
    const auto flooded = generate_voxel_volume(image, layout, settings);
    CHECK(flooded.at(1, 0, 0).kind == VoxelKind::Solid);
    CHECK(flooded.at(1, 1, 0).kind == VoxelKind::Water);
    CHECK(flooded.at(1, 5, 0).kind == VoxelKind::Water);
    CHECK(flooded.at(1, 6, 0).kind == VoxelKind::Air);

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
    CHECK(precise_out.columns[0].height == 8);
    CHECK(precise_out.columns[0].material_id == 12);

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
    CHECK(rejected);

    rejected = false;
    try {
        TerrainColumns invalid_columns{1, 1, {{0, 0, 16, 1, 0, 0, 255}}};
        (void)generate_voxel_volume(invalid_columns, settings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    ChannelLayout packed_layout;
    packed_layout.channels = {
        ChannelRole::Height16Msb, ChannelRole::Height16Lsb,
        ChannelRole::MaterialId, ChannelRole::BiomeWaterFlags,
    };
    TerrainSettings generated_settings;
    generated_settings.max_height = 56.0f;
    generated_settings.volume_height = 64;
    generated_settings.sea_level = 12;
    const auto generated = generate_synthetic_map(64, 64, 0xC0FFEEu, generated_settings);
    const auto generated_again = generate_synthetic_map(64, 64, 0xC0FFEEu, generated_settings);
    CHECK(generated.pixels.size() == 64u * 64u);
    bool has_water = false, has_river = false, has_multiple_heights = false;
    const auto first_height = (static_cast<unsigned>(generated.pixels[0].r) << 8u) | generated.pixels[0].g;
    for (std::size_t i = 0; i < generated.pixels.size(); ++i) {
        const auto& pixel = generated.pixels[i];
        const auto& same = generated_again.pixels[i];
        CHECK(pixel.r == same.r && pixel.g == same.g && pixel.b == same.b && pixel.a == same.a);
        const auto h = (static_cast<unsigned>(pixel.r) << 8u) | pixel.g;
        has_multiple_heights = has_multiple_heights || h != first_height;
        has_water = has_water || (pixel.a & 0x80u) != 0;
        has_river = has_river || (pixel.a & 0x20u) != 0;
    }
    CHECK(has_multiple_heights && has_water && has_river);

    const auto generated_columns = generate_columns(generated, packed_layout, generated_settings);
    CHECK(generated_columns.columns.size() == generated.pixels.size());
    const auto generated_volume = generate_voxel_volume(generated_columns, generated_settings);
    CHECK(generated_volume.voxels.size() == 64u * 64u * 64u);
    bool flags_survive = false;
    bool has_water_voxel = false;
    for (const auto& voxel : generated_volume.voxels) {
        has_water_voxel = has_water_voxel || voxel.kind == VoxelKind::Water;
    }
    for (const auto& column : generated_columns.columns) {
        const auto& top = generated_volume.at(column.x, column.height, column.z);
        CHECK(top.biome_id == column.biome_id);
        CHECK(top.feature_flags == column.feature_flags);
        flags_survive = flags_survive || top.feature_flags != 0;
    }
    CHECK(flags_survive);
    CHECK(has_water_voxel);

    // Exercise the persisted representation using a nontrivial generated map.
    const auto generated_voxel_path = std::filesystem::temp_directory_path() /
                                      "rgba_terrain_generated_roundtrip.rgba-voxels";
    const auto generated_write = write_voxel_file(
        generated_voxel_path.string(), generated_volume);
    REQUIRE(generated_write.has_value());
    const auto generated_read = read_voxel_file(generated_voxel_path.string());
    REQUIRE(generated_read.has_value());
    const auto generated_report = compare_voxel_volumes(
        generated_volume, *generated_read);
    CHECK(generated_report.exact_match());
    CHECK(generated_report.compared_voxels == generated_volume.voxels.size());
    std::error_code generated_cleanup_error;
    std::filesystem::remove(generated_voxel_path, generated_cleanup_error);

    rejected = false;
    try {
        ChannelLayout conflicting;
        conflicting.channels = {
            ChannelRole::Height16Msb, ChannelRole::Height16Lsb,
            ChannelRole::BiomeId, ChannelRole::BiomeWaterFlags,
        };
        (void)generate_columns(generated, conflicting, generated_settings);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);

    const auto metadata_path = std::filesystem::temp_directory_path() /
                               "rgba_terrain_packed_flags.rgba-terrain";
    {
        std::ofstream metadata_file(metadata_path);
        metadata_file
            << "channel.r=height16_msb\n"
            << "channel.g=height16_lsb\n"
            << "channel.b=material_id\n"
            << "channel.a=biome_water_flags\n"
            << "min_height=0\n"
            << "max_height=56\n"
            << "volume_height=64\n"
            << "sea_level=12\n";
        CHECK(static_cast<bool>(metadata_file));
    }
    const auto parsed_metadata = load_metadata(metadata_path.string());
    std::error_code cleanup_error;
    std::filesystem::remove(metadata_path, cleanup_error);
    CHECK(parsed_metadata.layout.channels[3] == ChannelRole::BiomeWaterFlags);
    CHECK(parsed_metadata.settings.max_height == 56.0f);
    const auto parsed_columns = generate_columns(
        generated, parsed_metadata.layout, parsed_metadata.settings);
    CHECK(parsed_columns.columns.size() == generated.pixels.size());


    const auto eight_bit_analysis = analyze_map(image, layout, settings);
    CHECK(eight_bit_analysis.height_encoding_max == 255);
    const auto sixteen_bit_analysis = analyze_map(
        precise, precise_layout, settings);
    CHECK(sixteen_bit_analysis.height_encoding_max == 65535);
    CHECK(sixteen_bit_analysis.min_height == 0x8000u);
    CHECK(sixteen_bit_analysis.max_height == 0x8000u);
    const auto analysis = analyze_map(
        generated, packed_layout, generated_settings);
    CHECK(analysis.width == 64 && analysis.depth == 64);
    CHECK(analysis.min_height <= analysis.max_height);
    CHECK(analysis.distinct_heights > 57);
    CHECK(analysis.max_height > 255);
    CHECK(analysis.standard_deviation > 0.0);
    CHECK(analysis.horizontal_edges == 64u * 63u);
    CHECK(analysis.vertical_edges == 64u * 63u);
    CHECK(analysis.water_columns > 0);
    CHECK(analysis.river_columns > 0);
    CHECK(analysis.materials[1] + analysis.materials[4] +
               analysis.materials[5] == 64u * 64u);
}
