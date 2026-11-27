#include <ws/rgba_terrain_core/world_plan.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

using namespace ws::rgba_terrain_core;

TEST_CASE("world plans validate finite worlds and independent spatial scales") {
    auto plan = make_default_world_plan(4000000, 2000000, 0xC0FFEEu);
    CHECK(plan.width == 4000000);
    CHECK(plan.depth == 2000000);
    CHECK(plan.scales.continent_scale > plan.scales.province_scale);
    CHECK(plan.scales.province_scale > plan.scales.ecosystem_scale);
    CHECK(plan.scales.ecosystem_scale > plan.scales.sub_biome_scale);
    CHECK(plan.scales.sub_biome_scale > plan.scales.habitat_scale);
    CHECK_NOTHROW(plan.validate());

    plan.scales.habitat_scale = 0.0;
    CHECK_THROWS_AS(plan.validate(), std::invalid_argument);

    plan = make_default_world_plan(1024, 1024, 7);
    plan.biomes.push_back(plan.biomes.back());
    CHECK_THROWS_AS(plan.validate(), std::invalid_argument);

    plan = make_default_world_plan(1024, 1024, 7);
    plan.biomes.front().id = 8;
    CHECK_THROWS_AS(plan.validate(), std::invalid_argument);
}

TEST_CASE("world sampler is deterministic at global coordinates") {
    WorldSampler sampler(make_default_world_plan(
        4000000, 2000000, 0x123456789ABCDEF0ull));

    const auto first = sampler.sample(1234567, 765432);
    const auto second = sampler.sample(1234567, 765432);
    CHECK(first == second);
    CHECK(first.normalized_elevation >= 0.0);
    CHECK(first.normalized_elevation <= 1.0);
    CHECK(first.humidity >= 0.0);
    CHECK(first.humidity <= 1.0);
    CHECK(first.precipitation >= 0.0);
    CHECK(first.precipitation <= 1.0);
    CHECK(first.biome_id == 0 || (first.biome_id >= 8 && first.biome_id <= 27));
    CHECK(first.ecosystem_id != 0);
}

TEST_CASE("gradient terrain fields vary spatially and remain order-independent") {
    auto plan = make_default_world_plan(256, 256, 0x6EAD1E17u);
    plan.scales.continent_scale = 160.0;
    plan.scales.province_scale = 64.0;
    plan.scales.ecosystem_scale = 40.0;
    plan.scales.sub_biome_scale = 18.0;
    plan.scales.habitat_scale = 6.0;
    plan.hydrology.routing_cell_scale = 12.0;
    plan.validate();

    const WorldSampler sampler(std::move(plan));
    constexpr std::array<std::array<std::int64_t, 2>, 9> coordinates{{
        {{24, 24}}, {{88, 24}}, {{152, 24}},
        {{24, 88}}, {{88, 88}}, {{152, 88}},
        {{24, 152}}, {{88, 152}}, {{152, 152}},
    }};

    std::array<WorldSample, coordinates.size()> forward{};
    std::array<WorldSample, coordinates.size()> reverse{};
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        forward[i] = sampler.sample(coordinates[i][0], coordinates[i][1]);
        const std::size_t reverse_index = coordinates.size() - 1u - i;
        reverse[reverse_index] = sampler.sample(
            coordinates[reverse_index][0], coordinates[reverse_index][1]);
    }

    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        CHECK(forward[i] == reverse[i]);
        CHECK(forward[i].normalized_elevation >= 0.0);
        CHECK(forward[i].normalized_elevation <= 1.0);
    }

    double horizontal_variation = 0.0;
    double vertical_variation = 0.0;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 2; ++column) {
            const std::size_t index = row * 3u + column;
            horizontal_variation += std::abs(
                forward[index + 1u].normalized_elevation -
                forward[index].normalized_elevation);
            vertical_variation += std::abs(
                forward[index + 3u].normalized_elevation -
                forward[index].normalized_elevation);
        }
    }
    CHECK(horizontal_variation > 1.0e-5);
    CHECK(vertical_variation > 1.0e-5);
}

TEST_CASE("world sampler rejects coordinates outside finite world bounds") {
    WorldSampler sampler(make_default_world_plan(256, 128, 42));
    CHECK_THROWS_AS((void)sampler.sample(-1, 0), std::out_of_range);
    CHECK_THROWS_AS((void)sampler.sample(256, 0), std::out_of_range);
    CHECK_THROWS_AS((void)sampler.sample(0, 128), std::out_of_range);
}

TEST_CASE("world regions softly influence temperature, elevation and ecosystems") {
    auto baseline_plan = make_default_world_plan(200000, 200000, 99);
    auto regional_plan = baseline_plan;
    WorldRegion region;
    region.id = 1;
    region.center_x = 100000.0;
    region.center_z = 100000.0;
    region.radius_x = 30000.0;
    region.radius_z = 22000.0;
    region.rotation_radians = 0.35;
    region.elevation_bias = 0.10;
    region.temperature_bias_c = 8.0;
    region.humidity_bias = -0.10;
    region.ecosystem_id = 3;
    region.ecosystem_affinity = 1.0;
    regional_plan.regions.push_back(region);

    const WorldSampler baseline(std::move(baseline_plan));
    const WorldSampler regional(std::move(regional_plan));
    const auto before = baseline.sample(100000, 100000);
    const auto after = regional.sample(100000, 100000);
    CHECK(after.normalized_elevation > before.normalized_elevation);
    CHECK(after.temperature_c > before.temperature_c);
    CHECK(after.humidity < before.humidity);
    CHECK(after.ecosystem_id == 3);
}

TEST_CASE("RGBA regions stitch exactly when generated as separate chunks") {
    const WorldSampler sampler(make_default_world_plan(1024, 1024, 0xBEEF));
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    const auto combined = sampler.generate_rgba_region(120, 240, 8, 6, settings);
    const auto left = sampler.generate_rgba_region(120, 240, 4, 6, settings);
    const auto right = sampler.generate_rgba_region(124, 240, 4, 6, settings);

    for (std::size_t z = 0; z < 6; ++z) {
        for (std::size_t x = 0; x < 4; ++x) {
            const auto& a = combined.at(x, z);
            const auto& b = left.at(x, z);
            CHECK(a.r == b.r);
            CHECK(a.g == b.g);
            CHECK(a.b == b.b);
            CHECK(a.a == b.a);

            const auto& c = combined.at(x + 4, z);
            const auto& d = right.at(x, z);
            CHECK(c.r == d.r);
            CHECK(c.g == d.g);
            CHECK(c.b == d.b);
            CHECK(c.a == d.a);
        }
    }

    ChannelLayout layout;
    layout.channels = {
        ChannelRole::Height16Msb,
        ChannelRole::Height16Lsb,
        ChannelRole::MaterialId,
        ChannelRole::BiomeWaterFlags,
    };
    const auto columns = generate_columns(combined, layout, settings);
    CHECK(columns.width == 8);
    CHECK(columns.depth == 6);
    const auto volume = generate_voxel_volume(columns, settings);
    CHECK(volume.width == 8);
    CHECK(volume.depth == 6);
    CHECK(volume.height == settings.volume_height);
}

TEST_CASE("coarse hydrology runoff responds to the shared precipitation climate") {
    auto dry_plan = make_default_world_plan(96, 96, 0xC11A7Eu);
    dry_plan.hydrology.routing_cell_scale = 4.0;
    dry_plan.hydrology.river_accumulation_threshold = 0.65;
    dry_plan.hydrology.lake_minimum_accumulation = 1.0e9;
    dry_plan.climate.base_humidity = 0.08;
    auto wet_plan = dry_plan;
    wet_plan.climate.base_humidity = 0.92;

    const WorldSampler dry(std::move(dry_plan));
    const WorldSampler wet(std::move(wet_plan));
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    const auto dry_map = dry.generate_rgba_region(0, 0, 96, 96, settings);
    const auto wet_map = wet.generate_rgba_region(0, 0, 96, 96, settings);
    std::size_t dry_river_cells = 0;
    std::size_t wet_river_cells = 0;
    for (std::size_t i = 0; i < dry_map.pixels.size(); ++i) {
        // RGBA v2 stores the river feature in bit 6 of the packed B byte.
        dry_river_cells += (dry_map.pixels[i].b & 0x40u) != 0u ? 1u : 0u;
        wet_river_cells += (wet_map.pixels[i].b & 0x40u) != 0u ? 1u : 0u;
    }
    CHECK(wet_river_cells >= dry_river_cells);
    CHECK(wet_map.pixels.size() == dry_map.pixels.size());
}

TEST_CASE("coarse hydrology remains identical across separately generated regions") {
    auto plan = make_default_world_plan(128, 128, 0xD4A1A6Eu);
    plan.hydrology.routing_cell_scale = 8.0;
    plan.hydrology.river_accumulation_threshold = 2.0;
    const WorldSampler sampler(std::move(plan));

    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    const auto combined = sampler.generate_rgba_region(16, 16, 32, 24, settings);
    const auto left = sampler.generate_rgba_region(16, 16, 16, 24, settings);
    const auto right = sampler.generate_rgba_region(32, 16, 16, 24, settings);
    for (std::size_t z = 0; z < 24; ++z) {
        for (std::size_t x = 0; x < 16; ++x) {
            const auto& whole_left = combined.at(x, z);
            const auto& split_left = left.at(x, z);
            CHECK(whole_left.r == split_left.r);
            CHECK(whole_left.g == split_left.g);
            CHECK(whole_left.b == split_left.b);
            CHECK(whole_left.a == split_left.a);

            const auto& whole_right = combined.at(x + 16, z);
            const auto& split_right = right.at(x, z);
            CHECK(whole_right.r == split_right.r);
            CHECK(whole_right.g == split_right.g);
            CHECK(whole_right.b == split_right.b);
            CHECK(whole_right.a == split_right.a);
        }
    }
}

TEST_CASE("sampler can generate a small region near the edge of a huge world") {
    const WorldSampler sampler(make_default_world_plan(
        4000000000ll, 2000000000ll, 0xFACE1234));
    const auto sample = sampler.sample(3999999999ll, 1999999999ll);
    CHECK(sample.normalized_elevation >= 0.0);
    CHECK(sample.normalized_elevation <= 1.0);

    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    const auto region = sampler.generate_rgba_region(
        3999999996ll, 1999999997ll, 4, 3, settings);
    CHECK(region.width == 4);
    CHECK(region.height == 3);
}

TEST_CASE("coupled climate diagnostics are deterministic and bounded") {
    const WorldSampler sampler(make_default_world_plan(200000, 200000, 0xC11A7Eu));
    bool found_land = false;
    for (std::int64_t z = 40000; z < 160000 && !found_land; z += 7919) {
        for (std::int64_t x = 40000; x < 160000; x += 6151) {
            const auto sample = sampler.sample(x, z);
            if (sample.is_ocean) {
                continue;
            }
            const auto repeated = sampler.sample(x, z);
            CHECK(sample == repeated);
            CHECK(sample.orographic_lift >= 0.0);
            CHECK(sample.orographic_lift <= 1.0);
            CHECK(sample.rain_shadow >= 0.0);
            CHECK(sample.rain_shadow <= 1.0);
            CHECK(sample.ocean_moisture >= 0.0);
            CHECK(sample.ocean_moisture <= 1.0);
            CHECK(sample.humidity >= 0.0);
            CHECK(sample.humidity <= 1.0);
            CHECK(sample.precipitation >= 0.0);
            CHECK(sample.precipitation <= 1.0);
            found_land = true;
            break;
        }
    }
    CHECK(found_land);
}

TEST_CASE("elevation cools climate through the configured lapse rate") {
    auto no_lapse_plan = make_default_world_plan(200000, 200000, 0xE1E7u);
    auto lapse_plan = no_lapse_plan;
    no_lapse_plan.climate.elevation_lapse_rate_c_per_km = 0.0;
    lapse_plan.climate.elevation_lapse_rate_c_per_km = 10.0;

    const WorldSampler no_lapse(std::move(no_lapse_plan));
    const WorldSampler with_lapse(std::move(lapse_plan));
    bool found_elevated_land = false;
    for (std::int64_t z = 30000; z < 170000 && !found_elevated_land; z += 7919) {
        for (std::int64_t x = 30000; x < 170000; x += 6151) {
            const auto baseline = no_lapse.sample(x, z);
            if (baseline.is_ocean || baseline.elevation_m < 500.0) {
                continue;
            }
            const auto cooled = with_lapse.sample(x, z);
            CHECK(cooled.temperature_c < baseline.temperature_c);
            CHECK(cooled.temperature_c ==
                  doctest::Approx(baseline.temperature_c -
                                  baseline.elevation_m / 1000.0 * 10.0));
            found_elevated_land = true;
            break;
        }
    }
    CHECK(found_elevated_land);
}

TEST_CASE("greater atmospheric moisture availability supports more precipitation") {
    auto dry_plan = make_default_world_plan(200000, 200000, 0xA7105u);
    auto wet_plan = dry_plan;
    dry_plan.climate.base_humidity = 0.12;
    wet_plan.climate.base_humidity = 0.82;

    const WorldSampler dry(std::move(dry_plan));
    const WorldSampler wet(std::move(wet_plan));
    bool found_land = false;
    for (std::int64_t z = 20000; z < 180000 && !found_land; z += 7919) {
        for (std::int64_t x = 20000; x < 180000; x += 6151) {
            const auto dry_sample = dry.sample(x, z);
            if (dry_sample.is_ocean) {
                continue;
            }
            const auto wet_sample = wet.sample(x, z);
            CHECK(wet_sample.humidity >= dry_sample.humidity);
            CHECK(wet_sample.precipitation >= dry_sample.precipitation);
            found_land = true;
            break;
        }
    }
    CHECK(found_land);
}

TEST_CASE("hydrology diagnostics are deterministic and bounded") {
    const WorldSampler sampler(make_default_world_plan(
        200000, 200000, 0xD4A1A6Eu));
    bool found_land = false;
    for (std::int64_t z = 45000; z < 155000 && !found_land; z += 7919) {
        for (std::int64_t x = 45000; x < 155000; x += 6151) {
            const auto first = sampler.sample(x, z);
            if (first.is_ocean) continue;
            const auto second = sampler.sample(x, z);
            CHECK(first == second);
            CHECK(first.watershed_accumulation >= 0.0);
            CHECK(first.river_strength >= 0.0);
            CHECK(first.river_strength <= 1.0);
            CHECK(first.lake_depth_m >= 0.0);
            bool valid_drainage =
                  static_cast<std::uint8_t>(first.drainage_direction) <= 7 ||
                  first.drainage_direction == DrainageDirection::Sink;
            CHECK(valid_drainage);
            CHECK(!(first.is_river && first.is_lake));
            found_land = true;
            break;
        }
    }
    CHECK(found_land);
}

TEST_CASE("hydrology settings reject invalid routing and water thresholds") {
    auto plan = make_default_world_plan(1024, 1024, 17);
    plan.hydrology.routing_cell_scale = 0.0;
    CHECK_THROWS_AS(plan.validate(), std::invalid_argument);

    plan = make_default_world_plan(1024, 1024, 17);
    plan.hydrology.lake_minimum_precipitation = 1.5;
    CHECK_THROWS_AS(plan.validate(), std::invalid_argument);
}

TEST_CASE("river and lake feature flags reconstruct inland surface water") {
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 46.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    TerrainColumns columns;
    columns.width = 2;
    columns.depth = 1;
    columns.columns = {
        {0, 0, 20, 1, 1, 0, 255, 0x20},
        {1, 0, 24, 2, 2, 0, 255, 0x40},
    };
    const auto volume = generate_voxel_volume(columns, settings);
    CHECK(volume.at(0, 20, 0).kind == VoxelKind::Solid);
    CHECK(volume.at(0, 21, 0).kind == VoxelKind::Water);
    CHECK(volume.at(1, 24, 0).kind == VoxelKind::Solid);
    CHECK(volume.at(1, 25, 0).kind == VoxelKind::Water);
    CHECK(volume.at(0, 22, 0).kind == VoxelKind::Air);
}

TEST_CASE("world region emits deterministic climate diagnostic preview layers") {
    auto plan = make_default_world_plan(128, 128, 0xC11A7Eu);
    plan.climate.equator_temperature_c = 28.0;
    plan.climate.pole_temperature_c = -8.0;
    const WorldSampler sampler(std::move(plan));

    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    WorldClimateDiagnosticImages diagnostics;
    const auto with_diagnostics = sampler.generate_rgba_region(
        40, 40, 3, 2, settings, &diagnostics);
    const auto without_diagnostics = sampler.generate_rgba_region(
        40, 40, 3, 2, settings);

    CHECK(with_diagnostics.pixels.size() == without_diagnostics.pixels.size());
    for (std::size_t i = 0; i < with_diagnostics.pixels.size(); ++i) {
        CHECK(with_diagnostics.pixels[i].r == without_diagnostics.pixels[i].r);
        CHECK(with_diagnostics.pixels[i].g == without_diagnostics.pixels[i].g);
        CHECK(with_diagnostics.pixels[i].b == without_diagnostics.pixels[i].b);
        CHECK(with_diagnostics.pixels[i].a == without_diagnostics.pixels[i].a);
    }

    const std::array<const RgbaImage*, 6> layers{
        &diagnostics.temperature, &diagnostics.humidity,
        &diagnostics.precipitation, &diagnostics.orographic_lift,
        &diagnostics.rain_shadow, &diagnostics.ocean_moisture};
    for (const auto* layer : layers) {
        CHECK(layer->width == 3);
        CHECK(layer->height == 2);
        CHECK(layer->pixels.size() == 6);
        for (const auto& pixel : layer->pixels) {
            CHECK(pixel.r == pixel.g);
            CHECK(pixel.g == pixel.b);
            CHECK(pixel.a == 255);
        }
    }
}


TEST_CASE("world region emits deterministic hydrology diagnostic preview layers") {
    auto plan = make_default_world_plan(128, 128, 0xD4A1A6Eu);
    plan.hydrology.routing_cell_scale = 8.0;
    const WorldSampler sampler(std::move(plan));

    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    WorldHydrologyDiagnosticImages diagnostics;
    const auto with_diagnostics = sampler.generate_rgba_region(
        16, 16, 24, 20, settings, nullptr, &diagnostics);
    const auto without_diagnostics = sampler.generate_rgba_region(
        16, 16, 24, 20, settings);
    REQUIRE(with_diagnostics.pixels.size() == without_diagnostics.pixels.size());
    for (std::size_t i = 0; i < with_diagnostics.pixels.size(); ++i) {
        CHECK(with_diagnostics.pixels[i].r == without_diagnostics.pixels[i].r);
        CHECK(with_diagnostics.pixels[i].g == without_diagnostics.pixels[i].g);
        CHECK(with_diagnostics.pixels[i].b == without_diagnostics.pixels[i].b);
        CHECK(with_diagnostics.pixels[i].a == without_diagnostics.pixels[i].a);
    }

    const std::array<const RgbaImage*, 5> layers{
        &diagnostics.flow_accumulation, &diagnostics.river_strength,
        &diagnostics.drainage_direction, &diagnostics.lake_depth,
        &diagnostics.lake_basins};
    for (const auto* layer : layers) {
        CHECK(layer->width == 24);
        CHECK(layer->height == 20);
        REQUIRE(layer->pixels.size() == 24u * 20u);
        for (const auto& pixel : layer->pixels) {
            CHECK(pixel.a == 255);
        }
    }

    WorldHydrologyDiagnosticImages left_diagnostics;
    WorldHydrologyDiagnosticImages right_diagnostics;
    (void)sampler.generate_rgba_region(
        16, 16, 12, 20, settings, nullptr, &left_diagnostics);
    (void)sampler.generate_rgba_region(
        28, 16, 12, 20, settings, nullptr, &right_diagnostics);
    const std::array<const RgbaImage*, 5> left_layers{
        &left_diagnostics.flow_accumulation,
        &left_diagnostics.river_strength,
        &left_diagnostics.drainage_direction,
        &left_diagnostics.lake_depth, &left_diagnostics.lake_basins};
    const std::array<const RgbaImage*, 5> right_layers{
        &right_diagnostics.flow_accumulation,
        &right_diagnostics.river_strength,
        &right_diagnostics.drainage_direction,
        &right_diagnostics.lake_depth, &right_diagnostics.lake_basins};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        for (std::size_t z = 0; z < 20; ++z) {
            for (std::size_t x = 0; x < 12; ++x) {
                const auto& combined_left = layers[layer]->at(x, z);
                const auto& split_left = left_layers[layer]->at(x, z);
                const auto& combined_right = layers[layer]->at(x + 12, z);
                const auto& split_right = right_layers[layer]->at(x, z);
                CHECK(split_left.r == combined_left.r);
                CHECK(split_left.g == combined_left.g);
                CHECK(split_left.b == combined_left.b);
                CHECK(split_left.a == combined_left.a);
                CHECK(split_right.r == combined_right.r);
                CHECK(split_right.g == combined_right.g);
                CHECK(split_right.b == combined_right.b);
                CHECK(split_right.a == combined_right.a);
            }
        }
    }
}

TEST_CASE("world plan exposes detailed biome definitions and v2 material packing") {
    const auto plan = make_default_world_plan(256, 256, 0xB10BEu);
    CHECK(plan.biomes.size() == 21);
    CHECK(plan.biomes.front().id == 0);
    CHECK(plan.biomes.back().id == 27);
    CHECK_NOTHROW(plan.validate());

    const WorldSampler sampler(plan);
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;
    const auto image = sampler.generate_rgba_region(0, 0, 32, 32, settings);
    ChannelLayout layout;
    layout.channels = {
        ChannelRole::Height16Msb, ChannelRole::Height16Lsb,
        ChannelRole::MaterialFeatureFlags, ChannelRole::BiomeId};
    const auto columns = generate_columns(image, layout, settings);
    REQUIRE(columns.columns.size() == 32u * 32u);
    for (const auto& column : columns.columns) {
        CHECK(column.biome_id == 0 || (column.biome_id >= 8 && column.biome_id <= 27));
        CHECK(column.material_id <= 15);
        CHECK((column.feature_flags & 0x87u) == 0);
    }
}
