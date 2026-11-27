#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>

namespace ws::image {

// Requested output layout. Source keeps the image's decoded channel count.
enum class Channels : std::uint8_t {
    source = 0,
    gray = 1,
    gray_alpha = 2,
    rgb = 3,
    rgba = 4,
};

enum class ErrorCode : std::int32_t {
    invalid_argument = 1,
    decode_failed = 2,
    invalid_dimensions = 3,
    unsupported_channels = 4,
    not_rgba = 5,
    unsupported_bit_depth = 6,
    encode_failed = 7,
};

// Tightly packed, interleaved, top-to-bottom pixels. channels is the decoded
// output count; source_channels is what the file or memory encoded.
// No color grading, gamma conversion, resampling or vertical flip is applied.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t channels = 0;
    std::uint8_t source_channels = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool valid() const noexcept;
};

struct LoadOptions {
    Channels output_channels = Channels::source;
};

// Decode common raster formats supported by stb_image. With source output,
// pixel bytes preserve the decoded source-channel values and count.
// Encoded memory is not retained after the call returns.
[[nodiscard]] ws::core::Result<Image> load_file(
    std::string_view path, LoadOptions options = {});
[[nodiscard]] ws::core::Result<Image> load_memory(
    std::span<const std::uint8_t> encoded, LoadOptions options = {});

// Strict data-image entry points: require 8-bit data with four encoded
// source channels. They never silently turn RGB/grayscale images into RGBA
// or truncate 16-bit-per-channel inputs.
[[nodiscard]] ws::core::Result<Image> load_rgba8_file(std::string_view path);
[[nodiscard]] ws::core::Result<Image> load_rgba8_memory(
    std::span<const std::uint8_t> encoded);

// Write exact interleaved RGBA8 bytes to a lossless PNG. No color conversion,
// resampling, channel rearrangement, or alpha premultiplication is performed.
[[nodiscard]] ws::core::Status write_rgba8_png_file(
    std::string_view path, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> pixels);

}  // namespace ws::image
