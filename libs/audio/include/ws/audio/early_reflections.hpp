#pragma once

// Geometry-independent early-reflection DSP. A scene supplies a small set
// of path delays and surface gains; this processor mixes them into caller-
// owned PCM buffers without allocation, locks, virtual calls or device I/O.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ws::audio {

inline constexpr std::size_t kMaxEarlyReflections = 8;
inline constexpr float kSpeedOfSoundMetersPerSecond = 343.0f;

struct ReflectionTap {
  std::uint32_t delay_frames = 0;
  float gain = 0.0f;
};

struct EarlyReflectionProfile {
  std::array<ReflectionTap, kMaxEarlyReflections> taps{};
  std::size_t count = 0;
  float dry_gain = 0.88f;
  float wet_gain = 0.42f;

  [[nodiscard]] bool add(ReflectionTap tap) noexcept {
    if (count >= taps.size() || tap.delay_frames == 0 ||
        !std::isfinite(tap.gain) || tap.gain <= 0.0f) {
      return false;
    }
    taps[count++] = ReflectionTap{
        tap.delay_frames, std::clamp(tap.gain, 0.0f, 1.0f)};
    return true;
  }

  [[nodiscard]] std::uint32_t output_frames(
      std::uint32_t input_frames) const noexcept {
    std::uint32_t max_delay = 0;
    for (std::size_t i = 0; i < count && i < taps.size(); ++i) {
      max_delay = std::max(max_delay, taps[i].delay_frames);
    }
    if (max_delay > std::numeric_limits<std::uint32_t>::max() - input_frames) {
      return 0;
    }
    return input_frames + max_delay;
  }
};

// Converts a one-way wall distance into a round-trip reflection delay.
// Reflectivity is a linear amplitude coefficient, not an absorption percent.
[[nodiscard]] inline ReflectionTap reflection_from_distance(
    float distance_meters, float reflectivity,
    std::uint32_t sample_rate) noexcept {
  if (!std::isfinite(distance_meters) || distance_meters <= 0.0f ||
      !std::isfinite(reflectivity) || reflectivity <= 0.0f ||
      sample_rate == 0) {
    return {};
  }
  const double frames =
      (2.0 * static_cast<double>(distance_meters) /
       static_cast<double>(kSpeedOfSoundMetersPerSecond)) *
      static_cast<double>(sample_rate);
  if (!std::isfinite(frames) || frames < 1.0 ||
      frames > static_cast<double>(
                   std::numeric_limits<std::uint32_t>::max())) {
    return {};
  }
  return ReflectionTap{
      static_cast<std::uint32_t>(std::lround(frames)),
      std::clamp(reflectivity, 0.0f, 1.0f)};
}

// Mixes dry PCM and delayed copies of the dry signal into output.
// channels must be 1 or 2; buffers must not overlap. Returns the number
// of output frames, or 0 for invalid input / insufficient output capacity.
// Samples are interleaved signed 16-bit PCM, matching SoundDesc.
[[nodiscard]] inline std::uint32_t process_early_reflections(
    const std::int16_t* input, std::uint32_t input_frames,
    std::uint32_t channels, std::int16_t* output,
    std::uint32_t output_capacity_frames,
    const EarlyReflectionProfile& profile) noexcept {
  if (input == nullptr || output == nullptr || input_frames == 0 ||
      (channels != 1 && channels != 2) || profile.count > profile.taps.size() ||
      !std::isfinite(profile.dry_gain) || !std::isfinite(profile.wet_gain)) {
    return 0;
  }
  const std::uint32_t frames = profile.output_frames(input_frames);
  if (frames == 0 || frames > output_capacity_frames) return 0;

  const float dry = std::clamp(profile.dry_gain, 0.0f, 1.0f);
  const float wet = std::clamp(profile.wet_gain, 0.0f, 1.0f);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
      float mixed = 0.0f;
      if (frame < input_frames) {
        mixed = static_cast<float>(input[frame * channels + channel]) * dry;
      }
      for (std::size_t tap_index = 0; tap_index < profile.count; ++tap_index) {
        const ReflectionTap tap = profile.taps[tap_index];
        if (frame >= tap.delay_frames) {
          const std::uint32_t source_frame = frame - tap.delay_frames;
          if (source_frame < input_frames) {
            mixed += static_cast<float>(
                         input[source_frame * channels + channel]) *
                     (wet * std::clamp(tap.gain, 0.0f, 1.0f));
          }
        }
      }
      mixed = std::clamp(mixed, -32768.0f, 32767.0f);
      output[frame * channels + channel] =
          static_cast<std::int16_t>(mixed);
    }
  }
  return frames;
}

}  // namespace ws::audio
