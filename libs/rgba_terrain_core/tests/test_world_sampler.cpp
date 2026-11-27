#include <ws/rgba_terrain_core/world_plan.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/image/image.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

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
    const bool biome_ok = (first.biome_id == 0) || (first.biome_id >= 8 && first.biome_id <= 27);
    CHECK(biome_ok);
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

TEST_CASE("whole-world erosion changes terrain deterministically and preserves chunk stitching") {
    auto eroded_plan = make_default_world_plan(96, 96, 0xE20510u);
    eroded_plan.scales.continent_scale = 160.0;
    eroded_plan.scales.province_scale = 64.0;
    eroded_plan.scales.ecosystem_scale = 40.0;
    eroded_plan.scales.sub_biome_scale = 18.0;
    eroded_plan.scales.habitat_scale = 6.0;
    eroded_plan.hydrology.routing_cell_scale = 4.0;
    eroded_plan.hydrology.river_accumulation_threshold = 1.8;
    eroded_plan.hydrology.erosion_enabled = true;
    auto baseline_plan = eroded_plan;
    baseline_plan.hydrology.erosion_enabled = false;

    const WorldSampler eroded(std::move(eroded_plan));
    const WorldSampler baseline(std::move(baseline_plan));
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    WorldHydrologyDiagnosticImages erosion_diagnostics;
    const auto terrain = eroded.generate_rgba_region(
        0, 0, 96, 96, settings, nullptr, &erosion_diagnostics);
    const auto repeated = eroded.generate_rgba_region(0, 0, 96, 96, settings);
    const auto un_eroded = baseline.generate_rgba_region(0, 0, 96, 96, settings);
    REQUIRE(terrain.pixels.size() == 96u * 96u);

    // Land-only erosion offsets must not leak through bilinear interpolation
    // into ocean pixels along the coastline.
    std::size_t ocean_pixels = 0;
    for (std::size_t i = 0; i < un_eroded.pixels.size(); ++i) {
        const auto& base_pixel = un_eroded.pixels[i];
        const double base_elevation =
            (static_cast<double>(base_pixel.r) * 256.0 + base_pixel.g) /
            65535.0;
        if (base_elevation <= baseline.plan().sea_level_fraction) {
            ++ocean_pixels;
            const auto& delta = erosion_diagnostics.erosion_delta.pixels[i];
            CHECK(delta.r == 0u);
            CHECK(delta.g == 0u);
            CHECK(delta.b == 0u);
        }
    }
    CHECK(ocean_pixels > 0u);

    // All land drainage must terminate at an ocean outlet when the finite
    // routing grid contains ocean. The shared route ordering also guarantees
    // that no mixed downhill/priority-parent cycle can exist.
    constexpr std::size_t routing_cell_scale = 4;
    constexpr std::size_t routing_width = 96 / routing_cell_scale;
    constexpr std::size_t routing_height = 96 / routing_cell_scale;
    constexpr std::size_t routing_cell_count =
        routing_width * routing_height;
    const std::array<int, 8> route_dx{{0, 1, 1, 1, 0, -1, -1, -1}};
    const std::array<int, 8> route_dz{{-1, -1, 0, 1, 1, 1, 0, -1}};
    const std::array<std::array<std::uint8_t, 3>, 8> direction_colors{{
        {{40, 110, 255}}, {{40, 210, 255}}, {{40, 230, 120}},
        {{180, 240, 50}}, {{255, 190, 40}}, {{255, 110, 40}},
        {{230, 50, 80}}, {{180, 60, 230}},
    }};
    const auto direction_from_pixel = [&](const Rgba8& pixel) {
        for (std::size_t d = 0; d < direction_colors.size(); ++d) {
            if (pixel.r == direction_colors[d][0] &&
                pixel.g == direction_colors[d][1] &&
                pixel.b == direction_colors[d][2]) {
                return static_cast<int>(d);
            }
        }
        return -1;
    };
    const auto output_elevation = [&](std::size_t x, std::size_t z) {
        const auto& pixel = terrain.at(x, z);
        return (static_cast<double>(pixel.r) * 256.0 + pixel.g) / 65535.0;
    };
    REQUIRE(erosion_diagnostics.drainage_direction.width == 96);
    REQUIRE(erosion_diagnostics.drainage_direction.height == 96);
    std::size_t land_routing_cells = 0;
    for (std::size_t start_z = 0; start_z < routing_height; ++start_z) {
        for (std::size_t start_x = 0; start_x < routing_width; ++start_x) {
            const std::size_t start_world_x =
                start_x * routing_cell_scale + routing_cell_scale / 2;
            const std::size_t start_world_z =
                start_z * routing_cell_scale + routing_cell_scale / 2;
            if (output_elevation(start_world_x, start_world_z) <=
                eroded.plan().sea_level_fraction) {
                continue;
            }
            ++land_routing_cells;

            std::size_t current_x = start_x;
            std::size_t current_z = start_z;
            bool reached_ocean = false;
            bool route_valid = false;
            for (std::size_t step = 0; step <= routing_cell_count; ++step) {
                const std::size_t world_x =
                    current_x * routing_cell_scale + routing_cell_scale / 2;
                const std::size_t world_z =
                    current_z * routing_cell_scale + routing_cell_scale / 2;
                const auto& direction_pixel =
                    erosion_diagnostics.drainage_direction.at(world_x, world_z);
                if (direction_pixel.r == 0u && direction_pixel.g == 0u &&
                    direction_pixel.b == 0u) {
                    reached_ocean =
                        output_elevation(world_x, world_z) <=
                        eroded.plan().sea_level_fraction;
                    route_valid = reached_ocean;
                    break;
                }

                const int direction = direction_from_pixel(direction_pixel);
                if (direction < 0) {
                    break;
                }
                const auto next_x = static_cast<std::int64_t>(current_x) +
                    route_dx[static_cast<std::size_t>(direction)];
                const auto next_z = static_cast<std::int64_t>(current_z) +
                    route_dz[static_cast<std::size_t>(direction)];
                if (next_x < 0 || next_z < 0 ||
                    next_x >= static_cast<std::int64_t>(routing_width) ||
                    next_z >= static_cast<std::int64_t>(routing_height)) {
                    break;
                }
                current_x = static_cast<std::size_t>(next_x);
                current_z = static_cast<std::size_t>(next_z);
            }
            CHECK(route_valid);
            CHECK(reached_ocean);
        }
    }
    CHECK(land_routing_cells > 0u);

    std::size_t changed_heights = 0;
    for (std::size_t i = 0; i < terrain.pixels.size(); ++i) {
        CHECK(terrain.pixels[i].r == repeated.pixels[i].r);
        CHECK(terrain.pixels[i].g == repeated.pixels[i].g);
        CHECK(terrain.pixels[i].b == repeated.pixels[i].b);
        CHECK(terrain.pixels[i].a == repeated.pixels[i].a);
        const auto height_changed =
            terrain.pixels[i].r != un_eroded.pixels[i].r ||
            terrain.pixels[i].g != un_eroded.pixels[i].g;
        changed_heights += height_changed ? 1u : 0u;
    }
    CHECK(changed_heights > 0u);
    const auto& erosion_preview = erosion_diagnostics.erosion_delta;
    std::size_t marked_erosion_cells = 0;
    for (const auto& pixel : erosion_preview.pixels) {
        marked_erosion_cells += (pixel.r > 0u || pixel.b > 0u) ? 1u : 0u;
    }
    CHECK(marked_erosion_cells > 0u);

    const auto left = eroded.generate_rgba_region(0, 0, 48, 96, settings);
    const auto right = eroded.generate_rgba_region(48, 0, 48, 96, settings);
    for (std::size_t z = 0; z < 96; ++z) {
        for (std::size_t x = 0; x < 48; ++x) {
            const auto& whole_left = terrain.at(x, z);
            const auto& split_left = left.at(x, z);
            const auto& whole_right = terrain.at(x + 48, z);
            const auto& split_right = right.at(x, z);
            CHECK(whole_left.r == split_left.r);
            CHECK(whole_left.g == split_left.g);
            CHECK(whole_left.b == split_left.b);
            CHECK(whole_left.a == split_left.a);
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

    const std::array<const RgbaImage*, 6> layers{
        &diagnostics.flow_accumulation, &diagnostics.river_strength,
        &diagnostics.drainage_direction, &diagnostics.lake_depth,
        &diagnostics.lake_basins, &diagnostics.erosion_delta};
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
    const std::array<const RgbaImage*, 6> left_layers{
        &left_diagnostics.flow_accumulation,
        &left_diagnostics.river_strength,
        &left_diagnostics.drainage_direction,
        &left_diagnostics.lake_depth, &left_diagnostics.lake_basins,
        &left_diagnostics.erosion_delta};
    const std::array<const RgbaImage*, 6> right_layers{
        &right_diagnostics.flow_accumulation,
        &right_diagnostics.river_strength,
        &right_diagnostics.drainage_direction,
        &right_diagnostics.lake_depth, &right_diagnostics.lake_basins,
        &right_diagnostics.erosion_delta};
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
        const bool biome_ok = (column.biome_id == 0) || (column.biome_id >= 8 && column.biome_id <= 27);
        CHECK(biome_ok);
        CHECK(column.material_id <= 15);
        CHECK((column.feature_flags & 0x87u) == 0);
    }
}

TEST_CASE("generate full 256x256 terrain with erosion and save output files") {
    // This test generates the production 256x256 terrain map with erosion
    // and saves all output files for manual inspection.
    auto plan = make_default_world_plan(256, 256, 0xC0FFEEu);
    plan.scales.continent_scale = 160.0;
    plan.scales.province_scale = 64.0;
    plan.scales.ecosystem_scale = 40.0;
    plan.scales.sub_biome_scale = 18.0;
    plan.scales.habitat_scale = 6.0;
    plan.hydrology.routing_cell_scale = 4.0;
    plan.hydrology.river_accumulation_threshold = 1.8;
    plan.hydrology.erosion_enabled = true;
    plan.validate();

    const WorldSampler sampler(std::move(plan));
    TerrainSettings settings;
    settings.min_height = 0.0f;
    settings.max_height = 56.0f;
    settings.volume_height = 64;
    settings.sea_level = 12;

    WorldClimateDiagnosticImages climate_diagnostics;
    WorldHydrologyDiagnosticImages hydrology_diagnostics;

    const auto image = sampler.generate_rgba_region(
        0, 0, 256, 256, settings,
        &climate_diagnostics, &hydrology_diagnostics);

    REQUIRE(image.width == 256);
    REQUIRE(image.height == 256);
    REQUIRE(image.pixels.size() == 256u * 256u);

    // Create output directory
    std::filesystem::create_directories("terrain_output");

    // Helper to save an RgbaImage as PNG
    auto save_image = [](const char* filename, const RgbaImage& img) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(img.pixels.size() * 4);
        for (const auto& pixel : img.pixels) {
            bytes.push_back(pixel.r);
            bytes.push_back(pixel.g);
            bytes.push_back(pixel.b);
            bytes.push_back(pixel.a);
        }
        auto status = ws::image::write_rgba8_png_file(
            filename, static_cast<std::uint32_t>(img.width),
            static_cast<std::uint32_t>(img.height), bytes);
        return status.has_value();
    };

    // Save main terrain PNG (RGBA v2 format)
    CHECK(save_image("terrain_output/terrain.png", image));

    // Save erosion diagnostic (key output to verify erosion is visible)
    CHECK(save_image("terrain_output/erosion-delta.png", hydrology_diagnostics.erosion_delta));

    // Save other diagnostic images
    CHECK(save_image("terrain_output/river-strength.png", hydrology_diagnostics.river_strength));
    CHECK(save_image("terrain_output/flow-accumulation.png", hydrology_diagnostics.flow_accumulation));
    CHECK(save_image("terrain_output/lake-depth.png", hydrology_diagnostics.lake_depth));
    CHECK(save_image("terrain_output/lake-basins.png", hydrology_diagnostics.lake_basins));
    CHECK(save_image("terrain_output/drainage-direction.png", hydrology_diagnostics.drainage_direction));
    CHECK(save_image("terrain_output/temperature.png", climate_diagnostics.temperature));
    CHECK(save_image("terrain_output/humidity.png", climate_diagnostics.humidity));
    CHECK(save_image("terrain_output/precipitation.png", climate_diagnostics.precipitation));
    CHECK(save_image("terrain_output/orographic-lift.png", climate_diagnostics.orographic_lift));
    CHECK(save_image("terrain_output/rain-shadow.png", climate_diagnostics.rain_shadow));
    CHECK(save_image("terrain_output/ocean-moisture.png", climate_diagnostics.ocean_moisture));

    // Verify erosion_delta has meaningful data (non-zero pixels)
    std::size_t erosion_pixels = 0;
    for (const auto& pixel : hydrology_diagnostics.erosion_delta.pixels) {
        if (pixel.r > 0 || pixel.g > 0 || pixel.b > 0 || pixel.a > 0) {
            erosion_pixels++;
        }
    }
    MESSAGE("Erosion delta non-zero pixels: " << erosion_pixels);
    CHECK(erosion_pixels > 0u);

    // Verify terrain uses v2 format (biome in A channel, material/flags in B)
    std::size_t v2_biome_pixels = 0;
    std::size_t v2_material_pixels = 0;
    for (const auto& pixel : image.pixels) {
        if (pixel.a != 0) v2_biome_pixels++;  // biome ID in alpha
        if ((pixel.b & 0xF0u) != 0 || (pixel.b & 0x0Fu) != 0) v2_material_pixels++;  // material/flags
    }
    MESSAGE("V2 biome pixels (A != 0): " << v2_biome_pixels);
    MESSAGE("V2 material/flags pixels (B != 0): " << v2_material_pixels);
    CHECK(v2_biome_pixels > 0u);
    CHECK(v2_material_pixels > 0u);

    // Verify the full normalized elevation range is preserved (from recent fix)
    double min_elev = 1.0, max_elev = 0.0;
    for (const auto& pixel : image.pixels) {
        const double elev = (pixel.r * 256.0 + pixel.g) / 65535.0;
        min_elev = std::min(min_elev, elev);
        max_elev = std::max(max_elev, elev);
    }
    MESSAGE("Elevation range: [" << min_elev << ", " << max_elev << "]");
    CHECK(min_elev < 0.1);  // Should reach near 0 (sea level)
    CHECK(max_elev > 0.4);  // Should reach reasonable mountain height (default plan peaks ~0.45)
}
