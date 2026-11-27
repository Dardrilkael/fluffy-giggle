#include "rgba_terrain/metadata.hpp"
#include "rgba_terrain/rgba_terrain.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void write_u32(std::ostream& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out.put(static_cast<char>((value >> shift) & 255u));
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr
            << "Usage: rgba_terrain_poc <input.png> <input.rgba-terrain> "
               "<output.voxcols>\n";
        return 2;
    }

    try {
        const auto metadata = rgba_terrain::load_metadata(argv[2]);
        const auto image = rgba_terrain::load_rgba_image(argv[1]);
        const auto terrain = rgba_terrain::generate_columns(
            image, metadata.layout, metadata.settings);

        std::ofstream out(argv[3], std::ios::binary);
        if (!out) {
            throw std::runtime_error("cannot create output file");
        }
        out.write("RGBAVX01", 8);
        write_u32(out, static_cast<std::uint32_t>(terrain.width));
        write_u32(out, static_cast<std::uint32_t>(terrain.depth));
        for (const auto& column : terrain.columns) {
            write_u32(out, static_cast<std::uint32_t>(column.x));
            write_u32(out, static_cast<std::uint32_t>(column.z));
            write_u32(out, column.height);
            out.put(static_cast<char>(column.material_id));
            out.put(static_cast<char>(column.biome_id));
            out.put(static_cast<char>(column.water_mask));
            out.put(static_cast<char>(column.density));
        }
        if (!out) {
            throw std::runtime_error("failed writing voxel-column output");
        }

        std::cout << "Converted " << terrain.width << "x" << terrain.depth
                  << " RGBA pixels into " << terrain.columns.size()
                  << " spatial voxel columns.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "RGBA terrain conversion failed: " << exception.what()
                  << "\n";
        return 1;
    }
}
