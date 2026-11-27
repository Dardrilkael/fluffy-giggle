#include <ws/rgba_terrain_core/voxel_file.hpp>
#include <ws/rgba_terrain_core/voxel_validation.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using ws::rgba_terrain_core::Voxel;
using ws::rgba_terrain_core::VoxelKind;
using ws::rgba_terrain_core::VoxelVolume;

std::filesystem::path test_path(const char* suffix) {
    return std::filesystem::temp_directory_path() /
           (std::string("ws_rgba_terrain_") + suffix + ".rgba-voxels");
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(static_cast<bool>(out));
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(static_cast<bool>(in));
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

VoxelVolume sample_volume() {
    VoxelVolume volume;
    volume.width = 3;
    volume.depth = 2;
    volume.height = 2;
    volume.voxels.resize(volume.width * volume.depth * volume.height);
    for (std::size_t i = 0; i < volume.voxels.size(); ++i) {
        volume.voxels[i] = Voxel{
            static_cast<VoxelKind>(i % 3),
            static_cast<std::uint8_t>(10 + i),
            static_cast<std::uint8_t>(20 + i),
            static_cast<std::uint8_t>(30 + i),
            static_cast<std::uint8_t>(40 + i),
        };
    }
    return volume;
}

}  // namespace

TEST_CASE("RGAVOX02 writes and reads every voxel field exactly") {
    using namespace ws::rgba_terrain_core;
    const auto path = test_path("roundtrip");
    const VoxelVolume expected = sample_volume();

    const auto written = write_voxel_file(path.string(), expected);
    REQUIRE(written.has_value());

    const auto loaded = read_voxel_file(path.string());
    REQUIRE(loaded.has_value());
    const auto report = compare_voxel_volumes(expected, *loaded);
    CHECK(report.exact_match());
    CHECK(report.compared_voxels == expected.voxels.size());
    CHECK(report.mismatched_voxels == 0);

    const auto bytes = read_bytes(path);
    REQUIRE(bytes.size() == kVoxelFileHeaderBytes +
                                expected.voxels.size() * kVoxelFileVoxelBytes);
    CHECK(std::string(bytes.begin(), bytes.begin() + 8) == "RGAVOX02");
    CHECK(bytes[8] == 3);
    CHECK(bytes[12] == 2);
    CHECK(bytes[16] == 2);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("RGAVOX02 rejects bad magic, truncated payload and trailing bytes") {
    using namespace ws::rgba_terrain_core;
    const auto path = test_path("corrupt");
    REQUIRE(write_voxel_file(path.string(), sample_volume()).has_value());
    const auto valid = read_bytes(path);

    auto bad_magic = valid;
    bad_magic[0] = static_cast<std::uint8_t>('X');
    write_bytes(path, bad_magic);
    const auto bad_header_result = read_voxel_file(path.string());
    CHECK_FALSE(bad_header_result.has_value());
    CHECK(bad_header_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::unsupported_version));

    auto truncated = valid;
    truncated.pop_back();
    write_bytes(path, truncated);
    const auto truncated_result = read_voxel_file(path.string());
    CHECK_FALSE(truncated_result.has_value());
    CHECK(truncated_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::truncated_file));

    auto trailing = valid;
    trailing.push_back(0);
    write_bytes(path, trailing);
    const auto trailing_result = read_voxel_file(path.string());
    CHECK_FALSE(trailing_result.has_value());
    CHECK(trailing_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::trailing_data));

    auto invalid_kind = valid;
    invalid_kind[kVoxelFileHeaderBytes] = 0xffu;
    write_bytes(path, invalid_kind);
    const auto invalid_kind_result = read_voxel_file(path.string());
    CHECK_FALSE(invalid_kind_result.has_value());
    CHECK(invalid_kind_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::invalid_voxel_kind));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("RGAVOX02 rejects invalid voxel kinds and malformed dimensions") {
    using namespace ws::rgba_terrain_core;
    const auto path = test_path("invalid-kind");
    auto invalid_kind = sample_volume();
    invalid_kind.voxels[2].kind = static_cast<VoxelKind>(255);
    const auto write_result = write_voxel_file(path.string(), invalid_kind);
    CHECK_FALSE(write_result.has_value());
    CHECK(write_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::invalid_voxel_kind));

    const auto good_path = test_path("invalid-dimensions");
    REQUIRE(write_voxel_file(good_path.string(), sample_volume()).has_value());
    auto bytes = read_bytes(good_path);
    bytes[8] = 0;
    bytes[9] = 0;
    bytes[10] = 0;
    bytes[11] = 0;
    write_bytes(good_path, bytes);
    const auto read_result = read_voxel_file(good_path.string());
    CHECK_FALSE(read_result.has_value());
    CHECK(read_result.error().code ==
          static_cast<std::int32_t>(VoxelFileError::invalid_dimensions));

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(good_path, ignored);
}

TEST_CASE("voxel comparison reports field-level mismatches and coordinates") {
    using namespace ws::rgba_terrain_core;
    const VoxelVolume expected = sample_volume();
    VoxelVolume actual = expected;
    Voxel& changed = actual.voxels[4];
    changed.kind = VoxelKind::Air;
    changed.material_id ^= 0xffu;
    changed.biome_id ^= 0xffu;
    changed.density ^= 0xffu;
    changed.feature_flags ^= 0xffu;

    const auto report = compare_voxel_volumes(expected, actual);
    CHECK_FALSE(report.exact_match());
    CHECK(report.dimensions_match);
    CHECK(report.compared_voxels == expected.voxels.size());
    CHECK(report.mismatched_voxels == 1);
    CHECK(report.field_mismatches[static_cast<std::size_t>(VoxelField::kind)] == 1);
    CHECK(report.field_mismatches[static_cast<std::size_t>(VoxelField::material)] == 1);
    CHECK(report.field_mismatches[static_cast<std::size_t>(VoxelField::biome)] == 1);
    CHECK(report.field_mismatches[static_cast<std::size_t>(VoxelField::density)] == 1);
    CHECK(report.field_mismatches[static_cast<std::size_t>(VoxelField::feature_flags)] == 1);
    REQUIRE(report.has_first_mismatch);
    CHECK(report.first_mismatch.x == 1);
    CHECK(report.first_mismatch.y == 0);
    CHECK(report.first_mismatch.z == 1);

    VoxelVolume wrong_dimensions = actual;
    ++wrong_dimensions.width;
    const auto dimension_report =
        compare_voxel_volumes(expected, wrong_dimensions);
    CHECK_FALSE(dimension_report.dimensions_match);
    CHECK_FALSE(dimension_report.exact_match());
    CHECK(dimension_report.compared_voxels == 0);
}
