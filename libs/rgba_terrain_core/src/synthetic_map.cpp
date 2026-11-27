#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/world_sampler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ws::rgba_terrain_core {

// Compatibility adapter for the original v1 API and channel contract.
// New terrain work belongs in WorldSampler; this function intentionally
// contains no independent noise, climate, biome, or river implementation.
RgbaImage generate_synthetic_map(std::size_t width, std::size_t depth,
                                 std::uint32_t seed,
                                 const TerrainSettings& settings) {
    if (width == 0 || depth == 0 ||
        width > std::numeric_limits<std::size_t>::max() / depth ||
        width > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max()) ||
        depth > static_cast<std::size_t>(
                    std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("synthetic map dimensions are invalid");
    }
    if (!std::isfinite(settings.min_height) ||
        !std::isfinite(settings.max_height) ||
        settings.min_height < 0.0f ||
        settings.max_height <= settings.min_height ||
        settings.volume_height == 0 ||
        settings.max_height >= static_cast<float>(settings.volume_height) ||
        settings.sea_level >= settings.volume_height) {
        throw std::invalid_argument("synthetic map terrain settings are invalid");
    }

    // Keep the old API's arbitrary output dimensions while using world-unit
    // feature scales. Changing image resolution therefore changes sampling
    // density, not the size of continents or climate regions.
    auto plan = make_default_world_plan(
        static_cast<std::int64_t>(width),
        static_cast<std::int64_t>(depth), seed);
    plan.scales.continent_scale = 133.12;
    plan.scales.province_scale = 58.88;
    plan.scales.ecosystem_scale = 30.72;
    plan.scales.sub_biome_scale = 11.52;
    plan.scales.habitat_scale = 3.84;
    plan.hydrology.routing_cell_scale = 8.0;
    plan.sea_level_fraction =
        (static_cast<double>(settings.sea_level) - settings.min_height) /
        (static_cast<double>(settings.max_height) - settings.min_height);
    // The v1 settings API permits a sea level at either endpoint. WorldPlan
    // requires an interior fraction, so reject only the impossible mapping.
    if (!(plan.sea_level_fraction > 0.0 &&
          plan.sea_level_fraction < 1.0)) {
        throw std::invalid_argument(
            "synthetic map sea level must lie strictly between min and max height");
    }

    const double sea_normalized = plan.sea_level_fraction;
    const WorldSampler sampler(std::move(plan));
    const RgbaImage modern = sampler.generate_rgba_region(
        0, 0, width, depth, settings);

    RgbaImage legacy{width, depth, std::vector<Rgba8>(width * depth)};
    for (std::size_t i = 0; i < modern.pixels.size(); ++i) {
        const Rgba8& source = modern.pixels[i];
        const std::uint8_t material_id = source.b & 0x0Fu;
        const std::uint8_t features = source.b >> 4u;
        const bool forest = (features & 0x01u) != 0;
        const bool mountain = (features & 0x02u) != 0;
        const bool river = (features & 0x04u) != 0;
        const bool lake = (features & 0x08u) != 0;
        const bool ocean = source.a == 0;
        const bool water = ocean || river || lake;

        std::uint8_t legacy_biome = 0;
        if (mountain) legacy_biome = 3;
        else if (material_id == 4 || material_id == 13) legacy_biome = 2;
        else if (forest) legacy_biome = 1;

        std::uint8_t legacy_material = 1;  // grass
        if (water || material_id == 4 || material_id == 13)
            legacy_material = 4;           // sand
        else if (mountain || material_id == 3 || material_id >= 5)
            legacy_material = 5;           // stone

        std::uint16_t packed_height = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(source.r) << 8u) | source.g);
        if (river) {
            const double normalized = std::min(
                static_cast<double>(packed_height) / 65535.0,
                sea_normalized);
            packed_height = static_cast<std::uint16_t>(
                std::clamp(std::lround(normalized * 65535.0), 0l, 65535l));
        }

        std::uint8_t flags = legacy_biome & 0x07u;
        if (forest) flags |= 0x08u;
        if (mountain) flags |= 0x10u;
        if (river) flags |= 0x20u;
        if (water) flags |= 0x80u;

        legacy.pixels[i] = {
            static_cast<std::uint8_t>(packed_height >> 8u),
            static_cast<std::uint8_t>(packed_height & 0xffu),
            legacy_material,
            flags,
        };
    }

    legacy.validate();
    return legacy;
}

}  // namespace ws::rgba_terrain_core
