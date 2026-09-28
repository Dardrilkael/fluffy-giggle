#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <ws/core/result.hpp>

namespace ws::audio {

// ws::audio's error codes (root CLAUDE.md, "Error handling"): every public
// failure returns a ws::core::Error with category "audio" and one of these
// as its code. The enum stays ws::audio's - core never knows these values.
enum class AudioError : std::int32_t {
  not_initialized = 1,    // engine call before init() / after shutdown()
  already_initialized = 2,
  file_open_failed = 3,
  wav_decode_failed = 4,
  wav_too_large = 5,      // past the uint32 handle/frame surface
  invalid_handle = 6,     // unknown, stale or never-issued handle
  invalid_sound_data = 7, // in-memory PCM desc fails validation
  too_many_sounds = 8,    // AudioEngine::kMaxSounds reached
  too_many_sources = 9,   // AudioEngine::kMaxSources reached
  device_open_failed = 10,
  backend_error = 11,     // an OpenAL call failed; message carries the AL error
};

[[nodiscard]] inline ws::core::Error make_audio_error(AudioError code,
                                                     std::string message) {
  return ws::core::make_error(static_cast<std::int32_t>(code), "audio",
                              std::move(message));
}

// unexpected<Error> that converts into any Result<T>:
//   return fail(AudioError::invalid_handle, "no such sound handle");
// Name lookup hides ws::core::fail inside this namespace on purpose: audio
// errors go through the enum, never raw int codes.
[[nodiscard]] inline ws::core::unexpected<ws::core::Error> fail(
    AudioError code, std::string message) {
  return ws::core::unexpected<ws::core::Error>(
      make_audio_error(code, std::move(message)));
}

}  // namespace ws::audio
