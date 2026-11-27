#include <ws/image/image.hpp>

#include <limits>
#include <memory>
#include <string>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace ws::image {
namespace {

using StbiPixels = std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>;

[[nodiscard]] ws::core::unexpected<ws::core::Error> error(
    ErrorCode code, std::string message) {
    return ws::core::fail(static_cast<std::int32_t>(code), "image",
                          std::move(message));
}

[[nodiscard]] int output_channel_count(Channels channels) {
    const int count = static_cast<int>(channels);
    return count >= 0 && count <= 4 ? count : -1;
}

[[nodiscard]] ws::core::Result<Image> copy_decoded(
    StbiPixels decoded, int width, int height, int source_channels,
    int output_channels) {
    if (!decoded) {
        const char* reason = stbi_failure_reason();
        return error(ErrorCode::decode_failed,
                     reason != nullptr ? reason : "stb_image decode failed");
    }
    if (width <= 0 || height <= 0) {
        return error(ErrorCode::invalid_dimensions,
                     "decoded image has zero or negative dimensions");
    }
    if (source_channels < 1 || source_channels > 4 ||
        output_channels < 1 || output_channels > 4) {
        return error(ErrorCode::unsupported_channels,
                     "decoded image channel count is outside 1..4");
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto c = static_cast<std::size_t>(output_channels);
    if (w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > std::numeric_limits<std::size_t>::max() / c) {
        return error(ErrorCode::invalid_dimensions,
                     "decoded image byte count overflows size_t");
    }

    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.channels = static_cast<std::uint8_t>(output_channels);
    image.source_channels = static_cast<std::uint8_t>(source_channels);
    const std::size_t byte_count = w * h * c;
    image.pixels.assign(decoded.get(), decoded.get() + byte_count);
    if (!image.valid()) {
        return error(ErrorCode::invalid_dimensions,
                     "decoded image failed internal layout validation");
    }
    return image;
}

[[nodiscard]] ws::core::Result<Image> require_rgba(Image image) {
    if (image.source_channels != 4 || image.channels != 4) {
        return error(ErrorCode::not_rgba,
                     "expected an image with four encoded source channels (RGBA)");
    }
    return image;
}

}  // namespace

bool Image::valid() const noexcept {
    if (width == 0 || height == 0 || channels < 1 || channels > 4) {
        return false;
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto c = static_cast<std::size_t>(channels);
    if (w > std::numeric_limits<std::size_t>::max() / h ||
        w * h > std::numeric_limits<std::size_t>::max() / c) {
        return false;
    }
    return pixels.size() == w * h * c;
}

ws::core::Result<Image> load_file(std::string_view path,
                                  LoadOptions options) {
    if (path.empty()) {
        return error(ErrorCode::invalid_argument, "image path is empty");
    }
    const int desired = output_channel_count(options.output_channels);
    if (desired < 0) {
        return error(ErrorCode::invalid_argument,
                     "requested output channel count must be 0..4");
    }

    const std::string null_terminated_path(path);
    int width = 0;
    int height = 0;
    int source_channels = 0;
    StbiPixels decoded(
        stbi_load(null_terminated_path.c_str(), &width, &height,
                  &source_channels, desired),
        &stbi_image_free);
    const int output_channels = desired == 0 ? source_channels : desired;
    return copy_decoded(std::move(decoded), width, height, source_channels,
                        output_channels);
}

ws::core::Result<Image> load_memory(std::span<const std::uint8_t> encoded,
                                    LoadOptions options) {
    if (encoded.empty()) {
        return error(ErrorCode::invalid_argument, "encoded image data is empty");
    }
    if (encoded.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return error(ErrorCode::invalid_argument,
                     "encoded image data exceeds stb_image's size limit");
    }
    const int desired = output_channel_count(options.output_channels);
    if (desired < 0) {
        return error(ErrorCode::invalid_argument,
                     "requested output channel count must be 0..4");
    }

    int width = 0;
    int height = 0;
    int source_channels = 0;
    StbiPixels decoded(
        stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                              &width, &height, &source_channels, desired),
        &stbi_image_free);
    const int output_channels = desired == 0 ? source_channels : desired;
    return copy_decoded(std::move(decoded), width, height, source_channels,
                        output_channels);
}

ws::core::Result<Image> load_rgba8_file(std::string_view path) {
    if (path.empty()) {
        return error(ErrorCode::invalid_argument, "image path is empty");
    }
    const std::string null_terminated_path(path);
    if (stbi_is_16_bit(null_terminated_path.c_str()) != 0) {
        return error(ErrorCode::unsupported_bit_depth,
                     "RGBA data maps must use 8-bit channels; pack 16-bit "
                     "height across two 8-bit channels instead");
    }
    auto loaded = load_file(path, LoadOptions{Channels::rgba});
    if (!loaded) {
        return ws::core::unexpected<ws::core::Error>(loaded.error());
    }
    return require_rgba(std::move(*loaded));
}

ws::core::Result<Image> load_rgba8_memory(
    std::span<const std::uint8_t> encoded) {
    if (encoded.empty()) {
        return error(ErrorCode::invalid_argument, "encoded image data is empty");
    }
    if (encoded.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return error(ErrorCode::invalid_argument,
                     "encoded image data exceeds stb_image's size limit");
    }
    if (stbi_is_16_bit_from_memory(encoded.data(),
                                   static_cast<int>(encoded.size())) != 0) {
        return error(ErrorCode::unsupported_bit_depth,
                     "RGBA data maps must use 8-bit channels; pack 16-bit "
                     "height across two 8-bit channels instead");
    }
    auto loaded = load_memory(encoded, LoadOptions{Channels::rgba});
    if (!loaded) {
        return ws::core::unexpected<ws::core::Error>(loaded.error());
    }
    return require_rgba(std::move(*loaded));
}

}  // namespace ws::image
