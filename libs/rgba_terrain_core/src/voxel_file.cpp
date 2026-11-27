#include <ws/rgba_terrain_core/voxel_file.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace ws::rgba_terrain_core {
namespace {

constexpr std::size_t kVoxelsPerIoBlock = 4096;
constexpr std::size_t kBytesPerIoBlock =
    kVoxelsPerIoBlock * kVoxelFileVoxelBytes;

[[nodiscard]] ws::core::unexpected<ws::core::Error> file_error(
    VoxelFileError code, std::string message) {
    return ws::core::fail(static_cast<std::int32_t>(code),
                          "rgba_terrain_core", std::move(message));
}

void store_u32_le(std::uint8_t* dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<std::uint8_t>(value & 0xffu);
    dst[1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
    dst[2] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
    dst[3] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

[[nodiscard]] std::uint32_t load_u32_le(const std::uint8_t* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8u) |
           (static_cast<std::uint32_t>(src[2]) << 16u) |
           (static_cast<std::uint32_t>(src[3]) << 24u);
}

[[nodiscard]] bool volume_cell_count(
    std::size_t width, std::size_t depth, std::size_t height,
    std::size_t& count) noexcept {
    if (width == 0 || depth == 0 || height == 0) {
        return false;
    }
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    if (width > limit / depth) {
        return false;
    }
    const std::size_t plane = width * depth;
    if (plane > limit / height) {
        return false;
    }
    count = plane * height;
    return true;
}

[[nodiscard]] bool valid_kind(std::uint8_t kind) noexcept {
    return kind <= static_cast<std::uint8_t>(VoxelKind::Water);
}

void encode_voxel(std::uint8_t* dst, const Voxel& voxel) noexcept {
    dst[0] = static_cast<std::uint8_t>(voxel.kind);
    dst[1] = voxel.material_id;
    dst[2] = voxel.biome_id;
    dst[3] = voxel.density;
    dst[4] = voxel.feature_flags;
}

[[nodiscard]] bool decode_voxel(
    const std::uint8_t* src, Voxel& voxel) noexcept {
    if (!valid_kind(src[0])) {
        return false;
    }
    voxel.kind = static_cast<VoxelKind>(src[0]);
    voxel.material_id = src[1];
    voxel.biome_id = src[2];
    voxel.density = src[3];
    voxel.feature_flags = src[4];
    return true;
}

}  // namespace

ws::core::Status write_voxel_file(
    std::string_view path, const VoxelVolume& volume) {
    if (path.empty()) {
        return file_error(VoxelFileError::invalid_argument,
                          "voxel output path is empty");
    }
    if (volume.width > std::numeric_limits<std::uint32_t>::max() ||
        volume.depth > std::numeric_limits<std::uint32_t>::max() ||
        volume.height > std::numeric_limits<std::uint32_t>::max()) {
        return file_error(VoxelFileError::invalid_dimensions,
                          "voxel dimensions exceed the RGAVOX02 uint32 limit");
    }

    std::size_t cell_count = 0;
    if (!volume_cell_count(volume.width, volume.depth, volume.height,
                           cell_count)) {
        return file_error(VoxelFileError::invalid_dimensions,
                          "voxel dimensions must be non-zero and fit size_t");
    }
    if (volume.voxels.size() != cell_count) {
        return file_error(VoxelFileError::invalid_dimensions,
                          "voxel storage size does not match volume dimensions");
    }
    for (std::size_t i = 0; i < cell_count; ++i) {
        if (!valid_kind(static_cast<std::uint8_t>(volume.voxels[i].kind))) {
            return file_error(
                VoxelFileError::invalid_voxel_kind,
                "voxel kind at linear index " + std::to_string(i) +
                    " is outside the RGAVOX02 range 0..2");
        }
    }

    std::array<std::uint8_t, kVoxelFileHeaderBytes> header{};
    std::memcpy(header.data(), kVoxelFileMagic.data(), kVoxelFileMagic.size());
    store_u32_le(header.data() + 8, static_cast<std::uint32_t>(volume.width));
    store_u32_le(header.data() + 12, static_cast<std::uint32_t>(volume.depth));
    store_u32_le(header.data() + 16, static_cast<std::uint32_t>(volume.height));

    std::ofstream out(std::string(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return file_error(VoxelFileError::open_failed,
                          "cannot create voxel output: " + std::string(path));
    }
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if (!out) {
        return file_error(VoxelFileError::write_failed,
                          "failed writing RGAVOX02 header: " + std::string(path));
    }

    std::array<std::uint8_t, kBytesPerIoBlock> buffer{};
    std::size_t offset = 0;
    while (offset < cell_count) {
        const std::size_t count =
            std::min(kVoxelsPerIoBlock, cell_count - offset);
        for (std::size_t i = 0; i < count; ++i) {
            encode_voxel(buffer.data() + i * kVoxelFileVoxelBytes,
                         volume.voxels[offset + i]);
        }
        const auto byte_count = count * kVoxelFileVoxelBytes;
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(byte_count));
        if (!out) {
            return file_error(VoxelFileError::write_failed,
                              "failed writing RGAVOX02 voxel payload: " +
                                  std::string(path));
        }
        offset += count;
    }

    out.flush();
    if (!out) {
        return file_error(VoxelFileError::write_failed,
                          "failed flushing RGAVOX02 output: " + std::string(path));
    }
    return {};
}

ws::core::Result<VoxelVolume> read_voxel_file(std::string_view path) {
    if (path.empty()) {
        return file_error(VoxelFileError::invalid_argument,
                          "voxel input path is empty");
    }

    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return file_error(VoxelFileError::open_failed,
                          "cannot open voxel file: " + std::string(path));
    }

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(
        std::filesystem::path(path), size_error);
    if (size_error) {
        return file_error(VoxelFileError::read_failed,
                          "cannot determine voxel file size: " + size_error.message());
    }
    if (file_size < kVoxelFileHeaderBytes) {
        return file_error(VoxelFileError::truncated_file,
                          "voxel file is shorter than the RGAVOX02 header");
    }

    std::array<std::uint8_t, kVoxelFileHeaderBytes> header{};
    in.read(reinterpret_cast<char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
    if (in.gcount() != static_cast<std::streamsize>(header.size())) {
        return file_error(VoxelFileError::truncated_file,
                          "could not read the complete RGAVOX02 header");
    }
    if (std::memcmp(header.data(), kVoxelFileMagic.data(),
                    kVoxelFileMagic.size()) != 0) {
        return file_error(VoxelFileError::unsupported_version,
                          "voxel file magic/version is not RGAVOX02");
    }

    VoxelVolume volume;
    volume.width = load_u32_le(header.data() + 8);
    volume.depth = load_u32_le(header.data() + 12);
    volume.height = load_u32_le(header.data() + 16);

    std::size_t cell_count = 0;
    if (!volume_cell_count(volume.width, volume.depth, volume.height,
                           cell_count)) {
        return file_error(VoxelFileError::invalid_dimensions,
                          "RGAVOX02 dimensions must all be non-zero and fit size_t");
    }
    if (cell_count >
        (std::numeric_limits<std::uintmax_t>::max() -
         kVoxelFileHeaderBytes) / kVoxelFileVoxelBytes) {
        return file_error(VoxelFileError::size_overflow,
                          "RGAVOX02 payload size overflows uintmax_t");
    }

    const std::uintmax_t expected_size =
        kVoxelFileHeaderBytes +
        static_cast<std::uintmax_t>(cell_count) * kVoxelFileVoxelBytes;
    if (file_size < expected_size) {
        return file_error(VoxelFileError::truncated_file,
                          "RGAVOX02 voxel payload is truncated");
    }
    if (file_size > expected_size) {
        return file_error(VoxelFileError::trailing_data,
                          "RGAVOX02 file contains unexpected trailing bytes");
    }
    if (cell_count > volume.voxels.max_size()) {
        return file_error(VoxelFileError::size_overflow,
                          "RGAVOX02 voxel count exceeds vector max_size");
    }

    try {
        volume.voxels.resize(cell_count);
    } catch (const std::bad_alloc&) {
        return file_error(VoxelFileError::allocation_failed,
                          "not enough memory to allocate RGAVOX02 voxel volume");
    } catch (const std::length_error&) {
        return file_error(VoxelFileError::size_overflow,
                          "RGAVOX02 voxel allocation exceeds container limits");
    }

    std::array<std::uint8_t, kBytesPerIoBlock> buffer{};
    std::size_t offset = 0;
    while (offset < cell_count) {
        const std::size_t count =
            std::min(kVoxelsPerIoBlock, cell_count - offset);
        const auto byte_count = count * kVoxelFileVoxelBytes;
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(byte_count));
        if (in.gcount() != static_cast<std::streamsize>(byte_count)) {
            return file_error(VoxelFileError::truncated_file,
                              "could not read the complete RGAVOX02 voxel payload");
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!decode_voxel(buffer.data() + i * kVoxelFileVoxelBytes,
                              volume.voxels[offset + i])) {
                return file_error(
                    VoxelFileError::invalid_voxel_kind,
                    "invalid voxel kind at linear index " +
                        std::to_string(offset + i));
            }
        }
        offset += count;
    }

    return volume;
}

}  // namespace ws::rgba_terrain_core
