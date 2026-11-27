#include "rgba_terrain/metadata.hpp"
#include "rgba_terrain/rgba_terrain.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 255u));
    }
}

std::uint32_t checked_u32(std::size_t value, const char* field) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds the file format limit");
    }
    return static_cast<std::uint32_t>(value);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr
            << "Usage: rgba_terrain_poc <input.png> <input.rgba-terrain> "
               "<output.rgba-voxels>\n";
        return 2;
    }

    try {
        const auto metadata = rgba_terrain::load_metadata(argv[2]);
        const auto image = rgba_terrain::load_rgba_image(argv[1]);
        const auto volume = rgba_terrain::generate_voxel_volume(
            image, metadata.layout, metadata.settings);

        std::ofstream out(argv[3], std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot create output file");
        }

        // RGAVOX01 v1: magic, width, depth, height, then four bytes per voxel.
        // Voxel order is X-fastest, then Z, then Y.
        out.write("RGAVOX01", 8);
        write_u32(out, checked_u32(volume.width, "width"));
        write_u32(out, checked_u32(volume.depth, "depth"));
        write_u32(out, checked_u32(volume.height, "height"));
        for (const auto& voxel : volume.voxels) {
            out.put(static_cast<char>(voxel.kind));
            out.put(static_cast<char>(voxel.material_id));
            out.put(static_cast<char>(voxel.biome_id));
            out.put(static_cast<char>(voxel.density));
        }
        if (!out) {
            throw std::runtime_error("failed writing voxel volume");
        }

        std::cout << "Generated " << volume.width << "x" << volume.depth
                  << "x" << volume.height << " voxel volume ("
                  << volume.voxels.size() << " voxels, "
                  << volume.voxels.size() * sizeof(rgba_terrain::Voxel)
                  << " bytes of voxel data).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "RGBA terrain generation failed: " << exception.what()
                  << "\n";
        return 1;
    }
}
