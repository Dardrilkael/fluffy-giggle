#pragma once

#include <cstdint>

#include "ai_terrain/heightmap.hpp"
#include "ai_terrain/parameter_profile.hpp"

namespace ai_terrain::test_terrain {

// Synthetic fixtures used only by this experiment's demo and tests.
[[nodiscard]] Heightmap flat(std::size_t width = 32, std::size_t depth = 32);
[[nodiscard]] Heightmap rolling(std::size_t width = 32, std::size_t depth = 32);
[[nodiscard]] Heightmap mountainous(std::size_t width = 32, std::size_t depth = 32);
[[nodiscard]] Heightmap from_profile(const ParameterProfile& profile,
                                     std::uint64_t seed,
                                     std::uint64_t region_index,
                                     std::size_t width = 32,
                                     std::size_t depth = 32);

}  // namespace ai_terrain::test_terrain
