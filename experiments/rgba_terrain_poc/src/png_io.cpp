#include "rgba_terrain/rgba_terrain.hpp"
#include <png.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace rgba_terrain {
RgbaImage load_png_rgba(const char* path) {
  if (!path) throw std::invalid_argument("PNG path is null");
  png_image image{}; image.version=PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image,path)) throw std::runtime_error(std::string("cannot read PNG: ")+image.message);
  image.format=PNG_FORMAT_RGBA;
  std::vector<png_byte> bytes(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image,nullptr,bytes.data(),0,nullptr)) {
    const std::string message=image.message; png_image_free(&image);
    throw std::runtime_error("cannot decode PNG: "+message);
  }
  RgbaImage out; out.width=image.width; out.height=image.height; out.pixels.resize(out.width*out.height);
  for (std::size_t i=0;i<out.pixels.size();++i) out.pixels[i]={bytes[i*4],bytes[i*4+1],bytes[i*4+2],bytes[i*4+3]};
  png_image_free(&image); out.validate(); return out;
}
} // namespace rgba_terrain
