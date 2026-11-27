#include <ws/rgba_terrain_core/rgba_terrain.hpp>

#include <ws/image/image.hpp>

#include <stdexcept>

namespace ws::rgba_terrain_core {

RgbaImage load_rgba_image(const std::string& path) {
    auto decoded = ws::image::load_rgba8_file(path);
    if (!decoded) {
        throw std::runtime_error("cannot load RGBA data image '" + path +
                                 "': " + decoded.error().message);
    }

    RgbaImage image;
    image.width = decoded->width;
    image.height = decoded->height;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);

    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        const std::size_t offset = i * 4;
        image.pixels[i] = Rgba8{
            decoded->pixels[offset],
            decoded->pixels[offset + 1],
            decoded->pixels[offset + 2],
            decoded->pixels[offset + 3],
        };
    }

    image.validate();
    return image;
}

}  // namespace ws::rgba_terrain_core
