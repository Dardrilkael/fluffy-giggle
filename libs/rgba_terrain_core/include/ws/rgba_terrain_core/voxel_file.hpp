#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <ws/core/result.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>

namespace ws::rgba_terrain_core {

// Version 2 stores a 20-byte little-endian header followed by five explicit
// bytes per voxel. The format is independent of sizeof(Voxel) and host ABI.
enum class VoxelFileError : std::int32_t {
    invalid_argument = 1,
    open_failed = 2,
    invalid_header = 3,
    unsupported_version = 4,
    invalid_dimensions = 5,
    size_overflow = 6,
    invalid_file_size = 7,
    truncated_file = 8,
    trailing_data = 9,
    invalid_voxel_kind = 10,
    read_failed = 11,
    write_failed = 12,
    allocation_failed = 13,
};

inline constexpr std::string_view kVoxelFileMagic = "RGAVOX02";
inline constexpr std::size_t kVoxelFileHeaderBytes = 20;
inline constexpr std::size_t kVoxelFileVoxelBytes = 5;

// Persist and load a voxel volume without depending on the in-memory layout
// or padding of Voxel. Errors are returned through ws::core::Status/Result.
[[nodiscard]] ws::core::Status write_voxel_file(
    std::string_view path, const VoxelVolume& volume);
[[nodiscard]] ws::core::Result<VoxelVolume> read_voxel_file(
    std::string_view path);

}  // namespace ws::rgba_terrain_core
