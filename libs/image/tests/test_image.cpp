#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include <ws/image/image.hpp>

TEST_CASE("RGBA PNG bytes and spatial layout are preserved") {
    // 1x1, 8-bit RGBA PNG with the exact pixel (0x12, 0x34, 0x56, 0x78).
    constexpr std::array<std::uint8_t, 70> png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
        0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
        0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
        0x54, 0x78, 0x9c, 0x63, 0x10, 0x32, 0x09, 0xab,
        0x00, 0x00, 0x02, 0x0d, 0x01, 0x15, 0xa9, 0x7e,
        0xa5, 0xc6, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
    };

    const auto result = ws::image::load_rgba8_memory(png);
    REQUIRE(result.has_value());
    CHECK(result->valid());
    CHECK(result->width == 1);
    CHECK(result->height == 1);
    CHECK(result->channels == 4);
    CHECK(result->source_channels == 4);
    REQUIRE(result->pixels.size() == 4);
    CHECK(result->pixels[0] == 0x12);
    CHECK(result->pixels[1] == 0x34);
    CHECK(result->pixels[2] == 0x56);
    CHECK(result->pixels[3] == 0x78);
}

TEST_CASE("RGBA loader rejects empty and malformed encoded input") {
    const std::array<std::uint8_t, 0> empty{};
    CHECK_FALSE(ws::image::load_rgba8_memory(empty).has_value());

    const std::array<std::uint8_t, 8> not_an_image = {
        0, 1, 2, 3, 4, 5, 6, 7
    };
    CHECK_FALSE(ws::image::load_rgba8_memory(not_an_image).has_value());
}
