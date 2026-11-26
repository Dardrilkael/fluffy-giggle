#pragma once

// Voxel-world adapter for ws::audio's generic early-reflection processor.
// This is the only layer that knows about blocks, materials and voxel rays.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <ws/audio/early_reflections.hpp>
#include <ws/math/vecmath.hpp>

#include "world.hpp"

namespace voxel::acoustics {

inline constexpr float kProbeDistance = 18.0f;

[[nodiscard]] inline float reflectivity(Block block) noexcept {
  switch (block) {
    case Block::glass: return 0.94f;
    case Block::stone: return 0.88f;
    case Block::cobblestone: return 0.90f;
    case Block::bricks: return 0.86f;
    case Block::wood: return 0.72f;
    case Block::planks: return 0.72f;
    case Block::coal_ore: return 0.88f;
    case Block::iron_ore: return 0.90f;
    case Block::dirt: return 0.58f;
    case Block::sand: return 0.43f;
    case Block::grass: return 0.48f;
    case Block::leaves: return 0.20f;
    default: return 0.0f;
  }
}

// Six short axis probes approximate the nearest room boundaries. Each hit
// contributes a single-bounce path with round-trip distance and a material-
// dependent amplitude. The fixed profile is cheap to rebuild at a sound event.
[[nodiscard]] inline ws::audio::EarlyReflectionProfile room_response(
    const World& world, ws::math::Vec3 listener,
    std::uint32_t sample_rate) noexcept {
  constexpr float directions[6][3] = {
      {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
  const float origin[3] = {listener.x, listener.y, listener.z};

  ws::audio::EarlyReflectionProfile profile{};
  profile.dry_gain = 0.92f;
  profile.wet_gain = 0.36f;

  for (const auto& direction : directions) {
    const World::RayHit hit = world.raycast(
        origin, direction, kProbeDistance);
    if (!hit.hit || hit.distance < 0.25f) continue;

    const Block surface = world.block_at(
        hit.cell.x, hit.cell.y, hit.cell.z);
    const float material_gain = reflectivity(surface);
    if (material_gain <= 0.0f) continue;

    // Geometric spreading plus surface absorption. The audio library
    // applies the actual delay and PCM mix; this adapter supplies only data.
    const float gain = material_gain / (1.0f + 0.12f * hit.distance);
    const auto tap = ws::audio::reflection_from_distance(
        hit.distance, gain, sample_rate);
    (void)profile.add(tap);
  }

  // Stable chronological order makes the response reproducible regardless
  // of the iteration order a future probe provider might use.
  for (std::size_t i = 1; i < profile.count; ++i) {
    const auto value = profile.taps[i];
    std::size_t j = i;
    while (j > 0 &&
           profile.taps[j - 1].delay_frames > value.delay_frames) {
      profile.taps[j] = profile.taps[j - 1];
      --j;
    }
    profile.taps[j] = value;
  }

  // Keep coincident reflections from a small room below a controlled
  // combined amplitude. The processor remains a literal sum of taps.
  float combined_gain = 0.0f;
  for (std::size_t i = 0; i < profile.count; ++i) {
    combined_gain += profile.taps[i].gain;
  }
  constexpr float kMaxCombinedReflectionGain = 0.75f;
  if (combined_gain > 0.0f) {
    profile.wet_gain = std::min(
        profile.wet_gain, kMaxCombinedReflectionGain / combined_gain);
  }
  return profile;
}

}  // namespace voxel::acoustics
