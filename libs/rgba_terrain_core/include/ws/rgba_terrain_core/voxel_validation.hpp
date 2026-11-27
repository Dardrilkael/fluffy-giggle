#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <ws/rgba_terrain_core/rgba_terrain.hpp>

namespace ws::rgba_terrain_core {

enum class VoxelField : std::size_t {
    kind = 0,
    material = 1,
    biome = 2,
    density = 3,
    feature_flags = 4,
    count = 5,
};

struct VoxelMismatch {
    std::size_t x = 0;
    std::size_t y = 0;
    std::size_t z = 0;
    Voxel expected{};
    Voxel actual{};
};

struct VoxelValidationReport {
    bool dimensions_match = false;
    bool expected_storage_valid = false;
    bool actual_storage_valid = false;
    std::uint64_t compared_voxels = 0;
    std::uint64_t mismatched_voxels = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(VoxelField::count)>
        field_mismatches{};
    bool has_first_mismatch = false;
    VoxelMismatch first_mismatch{};

    [[nodiscard]] bool exact_match() const noexcept {
        return dimensions_match && expected_storage_valid &&
               actual_storage_valid && mismatched_voxels == 0;
    }
};

// Compares dimensions and every persisted voxel attribute. If dimensions or
// backing storage are invalid, voxel comparison is skipped.
[[nodiscard]] VoxelValidationReport compare_voxel_volumes(
    const VoxelVolume& expected, const VoxelVolume& actual) noexcept;

}  // namespace ws::rgba_terrain_core
