#include <ws/image/image.hpp>

#include <limits>
#include <string>
#include <utility>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ws::image {
namespace {

[[nodiscard]] ws::core::unexpected<ws::core::Error> encode_error(
    ErrorCode code, std::string message) {
    return ws::core::fail(static_cast<std::int32_t>(code), "image",
                          std::move(message));
}

}  // namespace

ws::core::Status write_rgba8_png_file(
    std::string_view path, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> pixels) {
    if (path.empty()) {
        return encode_error(ErrorCode::invalid_argument,
                            "PNG output path is empty");
    }
    if (width == 0 || height == 0 ||
        width > static_cast<std::uint32_t>(
                    std::numeric_limits<int>::max() / 4) ||
        height > static_cast<std::uint32_t>(
                     std::numeric_limits<int>::max())) {
        return encode_error(ErrorCode::invalid_dimensions,
                            "PNG dimensions are zero or exceed encoder limits");
    }

    const std::size_t w = width;
    const std::size_t h = height;
    if (w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > std::numeric_limits<std::size_t>::max() / 4u) {
        return encode_error(ErrorCode::invalid_dimensions,
                            "RGBA PNG byte count overflows size_t");
    }
    const std::size_t expected = w * h * 4u;
    if (pixels.size() != expected) {
        return encode_error(ErrorCode::invalid_argument,
                            "RGBA PNG byte count does not match dimensions");
    }

    const std::string null_terminated_path(path);
    const int stride = static_cast<int>(width * 4u);
    if (stbi_write_png(null_terminated_path.c_str(),
                       static_cast<int>(width), static_cast<int>(height),
                       4, pixels.data(), stride) == 0) {
        return encode_error(ErrorCode::encode_failed,
                            "stb_image_write could not encode PNG file");
    }
    return {};
}

}  // namespace ws::image
