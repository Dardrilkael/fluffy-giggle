#include <ws/rgba_terrain_core/world_plan.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <doctest/doctest.h>

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
    CHECK(first.biome_id <= 7);
    CHECK(first.ecosystem_id != 0);
}

TEST_CASE("world sampler rejects coordinates outside finite world bounds") {
    WorldSampler sampler(make_default_world_plan(256, 128, 42));
    CHECK_THROWS_AS(sampler.sample(-1, 0), std::out_of_range);
    CHECK_THROWS_AS(sampler.sample(256, 0), std::out_of_range);
    CHECK_THROWS_AS(sampler.sample(0, 128), std::out_of_range);
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
    region.humidity_bias = 0.35;
    region.ecosystem_id = 1;
    region.ecosystem_affinity = 0.65;
    regional_plan.regions.push_back(region);

    const WorldSampler baseline(std::move(baseline_plan));
    const WorldSampler regional(std::move(regional_plan));
    const auto before = baseline.sample(100000, 100000);
    const auto after = regional.sample(100000, 100000);
    CHECK(after.normalized_elevation > before.normalized_elevation);
    CHECK(after.temperature_c > before.temperature_c);
    CHECK(after.humidity >= before.humidity);
    CHECK(after.ecosystem_id == 1);
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
