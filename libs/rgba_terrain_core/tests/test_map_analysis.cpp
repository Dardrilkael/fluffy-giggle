#include <ws/rgba_terrain_core/map_analysis.hpp>

#include <doctest/doctest.h>

using namespace ws::rgba_terrain_core;

namespace {

RgbaImage make_analysis_image() {
    RgbaImage image;
    image.width = 3;
    image.height = 2;
    image.pixels = {
        {0x10, 0x00, 1, 0x09}, {0x20, 0x00, 1, 0x09},
        {0x30, 0x00, 2, 0x12},
        {0x40, 0x00, 2, 0xA2}, {0x50, 0x00, 2, 0xA2},
        {0x60, 0x00, 3, 0xC3},
    };
    return image;
}

ChannelLayout analysis_layout() {
    ChannelLayout layout;
    layout.channels = {
        ChannelRole::Height16Msb,
        ChannelRole::Height16Lsb,
        ChannelRole::MaterialId,
        ChannelRole::BiomeWaterFlags,
    };
    return layout;
}

TerrainSettings analysis_settings() {
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    return settings;
}

}  // namespace

TEST_CASE("map analysis reports encoded elevation histograms and slope percentiles") {
    const auto image = make_analysis_image();
    const auto analysis = analyze_map(
        image, analysis_layout(), analysis_settings());

    CHECK(analysis.width == 3);
    CHECK(analysis.depth == 2);
    CHECK(analysis.min_height == 0x1000);
    CHECK(analysis.max_height == 0x6000);
    CHECK(analysis.distinct_heights == 6);
    CHECK(analysis.elevation_histogram[1] == 1);
    CHECK(analysis.elevation_histogram[6] == 1);
    CHECK(analysis.mean_slope_degrees >= 0.0);
    CHECK(analysis.slope_p50_degrees <= analysis.slope_p90_degrees);
    CHECK(analysis.slope_p90_degrees <= analysis.slope_p95_degrees);
    CHECK(analysis.slope_p95_degrees <= analysis.slope_p99_degrees);
    CHECK(analysis.slope_p99_degrees <= 90.0);
    CHECK(analysis.slope_histogram[0] + analysis.slope_histogram[1] +
          analysis.slope_histogram[2] + analysis.slope_histogram[3] +
          analysis.slope_histogram[4] + analysis.slope_histogram[5] +
          analysis.slope_histogram[6] + analysis.slope_histogram[7] +
          analysis.slope_histogram[8] + analysis.slope_histogram[9] +
          analysis.slope_histogram[10] + analysis.slope_histogram[11] +
          analysis.slope_histogram[12] + analysis.slope_histogram[13] +
          analysis.slope_histogram[14] + analysis.slope_histogram[15] == 6);
}

TEST_CASE("map analysis measures 8-connected biome and feature regions") {
    const auto analysis = analyze_map(
        make_analysis_image(), analysis_layout(), analysis_settings());

    CHECK(analysis.forest_columns == 2);
    CHECK(analysis.river_columns == 2);
    CHECK(analysis.lake_columns == 1);
    CHECK(analysis.water_columns == 3);
    CHECK(analysis.forest_regions.component_count == 1);
    CHECK(analysis.forest_regions.largest_component == 2);
    CHECK(analysis.river_regions.component_count == 1);
    CHECK(analysis.river_regions.largest_component == 2);
    CHECK(analysis.lake_regions.component_count == 1);
    CHECK(analysis.water_regions.component_count == 1);
    CHECK(analysis.biome_regions[1].largest_component == 2);
}

TEST_CASE("diagnostic maps are separate images with original map dimensions") {
    const auto source = make_analysis_image();
    const auto previews = make_diagnostic_images(
        source, analysis_layout(), analysis_settings());

    CHECK(previews.elevation.width == source.width);
    CHECK(previews.elevation.height == source.height);
    CHECK(previews.slope.pixels.size() == source.pixels.size());
    CHECK(previews.biome.pixels.size() == source.pixels.size());
    CHECK(previews.material.pixels.size() == source.pixels.size());
    CHECK(previews.water.pixels.size() == source.pixels.size());
    CHECK(previews.forest.pixels.size() == source.pixels.size());
    CHECK(previews.mountain.pixels.size() == source.pixels.size());
    CHECK(previews.river.pixels.size() == source.pixels.size());
    CHECK(previews.lake.pixels.size() == source.pixels.size());
    CHECK(previews.forest.at(0, 0).r == 255);
    CHECK(previews.forest.at(2, 0).r == 0);
    CHECK(previews.river.at(0, 1).r == 255);
    CHECK(previews.lake.at(2, 1).r == 255);
    CHECK(source.pixels[0].r == 0x10);
}
