#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include <ws/audio/early_reflections.hpp>

TEST_CASE("audio reflections: an impulse yields correctly delayed echoes") {
  const std::array<std::int16_t, 4> dry{10000, 0, 0, 0};
  std::array<std::int16_t, 8> wet{};
  ws::audio::EarlyReflectionProfile profile{};
  profile.dry_gain = 1.0f;
  profile.wet_gain = 0.5f;
  REQUIRE(profile.add({2, 0.5f}));
  const auto frames = ws::audio::process_early_reflections(
      dry.data(), static_cast<std::uint32_t>(dry.size()), 1,
      wet.data(), static_cast<std::uint32_t>(wet.size()), profile);
  CHECK(frames == 6);
  CHECK(wet[0] == 10000);
  CHECK(wet[2] == 2500);
}

TEST_CASE("audio reflections: distance conversion is round-trip propagation") {
  const auto tap = ws::audio::reflection_from_distance(10.0f, 0.75f, 44100);
  CHECK(tap.delay_frames == 2572);
  CHECK(tap.gain == doctest::Approx(0.75f));
}

TEST_CASE("audio reflections: output capacity and invalid channels are rejected") {
  const std::int16_t input[] = {1000, 0};
  std::int16_t output[2]{};
  ws::audio::EarlyReflectionProfile profile{};
  REQUIRE(profile.add({4, 0.5f}));
  CHECK(ws::audio::process_early_reflections(
            input, 2, 1, output, 2, profile) == 0);
  CHECK(ws::audio::process_early_reflections(
            input, 2, 3, output, 2, profile) == 0);
}
