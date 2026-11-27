#include <ws/rgba_terrain_core/voxel_validation.hpp>

#include <limits>

namespace ws::rgba_terrain_core {
namespace {

[[nodiscard]] bool valid_storage(const VoxelVolume& volume) noexcept {
    if (volume.width == 0 || volume.depth == 0 || volume.height == 0) {
        return false;
    }
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    if (volume.width > limit / volume.depth) {
        return false;
    }
    const std::size_t plane = volume.width * volume.depth;
    if (plane > limit / volume.height) {
        return false;
    }
    return volume.voxels.size() == plane * volume.height;
}

}  // namespace

VoxelValidationReport compare_voxel_volumes(
    const VoxelVolume& expected, const VoxelVolume& actual) noexcept {
    VoxelValidationReport report;
    report.dimensions_match =
        expected.width == actual.width &&
        expected.depth == actual.depth &&
        expected.height == actual.height;
    report.expected_storage_valid = valid_storage(expected);
    report.actual_storage_valid = valid_storage(actual);
    if (!report.dimensions_match || !report.expected_storage_valid ||
        !report.actual_storage_valid) {
        return report;
    }

    report.compared_voxels = static_cast<std::uint64_t>(expected.voxels.size());
    constexpr std::size_t kKind = static_cast<std::size_t>(VoxelField::kind);
    constexpr std::size_t kMaterial = static_cast<std::size_t>(VoxelField::material);
    constexpr std::size_t kBiome = static_cast<std::size_t>(VoxelField::biome);
    constexpr std::size_t kDensity = static_cast<std::size_t>(VoxelField::density);
    constexpr std::size_t kFlags = static_cast<std::size_t>(VoxelField::feature_flags);

    for (std::size_t i = 0; i < expected.voxels.size(); ++i) {
        const Voxel& lhs = expected.voxels[i];
        const Voxel& rhs = actual.voxels[i];
        bool mismatch = false;

        if (lhs.kind != rhs.kind) {
            ++report.field_mismatches[kKind];
            mismatch = true;
        }
        if (lhs.material_id != rhs.material_id) {
            ++report.field_mismatches[kMaterial];
            mismatch = true;
        }
        if (lhs.biome_id != rhs.biome_id) {
            ++report.field_mismatches[kBiome];
            mismatch = true;
        }
        if (lhs.density != rhs.density) {
            ++report.field_mismatches[kDensity];
            mismatch = true;
        }
        if (lhs.feature_flags != rhs.feature_flags) {
            ++report.field_mismatches[kFlags];
            mismatch = true;
        }

        if (!mismatch) {
            continue;
        }
        ++report.mismatched_voxels;
        if (!report.has_first_mismatch) {
            const std::size_t plane = expected.width * expected.depth;
            report.has_first_mismatch = true;
            report.first_mismatch.x = i % expected.width;
            report.first_mismatch.z = (i / expected.width) % expected.depth;
            report.first_mismatch.y = i / plane;
            report.first_mismatch.expected = lhs;
            report.first_mismatch.actual = rhs;
        }
    }
    return report;
}

}  // namespace ws::rgba_terrain_core
