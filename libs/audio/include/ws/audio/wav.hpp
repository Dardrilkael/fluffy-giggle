#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <ws/audio/errors.hpp>
#include <ws/core/result.hpp>

namespace ws::audio {

// A decoded WAV: interleaved 16-bit PCM plus the format facts needed to
// play it. Ws-owned - dr_wav types never appear here (dr_wav is private to
// src/, like every third-party in the workspace).
struct WavSound {
  std::vector<std::int16_t> samples;  // interleaved, frame_count() * channels
  std::uint32_t channels = 0;
  std::uint32_t sample_rate = 0;

  [[nodiscard]] std::uint32_t frame_count() const noexcept {
    return channels == 0
               ? 0
               : static_cast<std::uint32_t>(samples.size() / channels);
  }
};

// Decodes a RIFF/WAVE file (any bit depth dr_wav converts from; the result
// is always 16-bit PCM). Errors carry category "audio".
[[nodiscard]] ws::core::Result<WavSound> load_wav(std::string_view path);

}  // namespace ws::audio
