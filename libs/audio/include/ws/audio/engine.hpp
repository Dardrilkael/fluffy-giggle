#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <ws/audio/errors.hpp>
#include <ws/core/result.hpp>

namespace ws::audio {

// Opaque ids into the engine's tables. 0 is never issued: a
// default-constructed handle is invalid and every API rejects it with
// invalid_handle. Ids are monotonic per init session - a reaped handle is
// stale forever, never recycled.
struct SoundHandle {
  std::uint32_t value = 0;
  [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

struct SourceHandle {
  std::uint32_t value = 0;
  [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

// Raw PCM handed to the engine: the bridge from WavSound (load_wav) or a
// synth. Interleaved 16-bit, 1 or 2 channels. The engine copies the data on
// load, so the memory may die with the descriptor.
struct SoundDesc {
  const std::int16_t* samples = nullptr;
  std::uint32_t frame_count = 0;
  std::uint32_t channels = 0;    // 1 or 2
  std::uint32_t sample_rate = 0;  // Hz
};

// The audio device + every buffer and source it issued. OpenAL Soft stays
// behind the pimpl: only src/ includes AL/*. One engine at a time in a
// process - the ALC context is process-global, so a second live engine
// would steal it from the first.
class AudioEngine {
 public:
  AudioEngine();
  ~AudioEngine();

  AudioEngine(const AudioEngine&) = delete;
  AudioEngine& operator=(const AudioEngine&) = delete;
  AudioEngine(AudioEngine&&) = delete;
  AudioEngine& operator=(AudioEngine&&) = delete;

  static constexpr std::uint32_t kMaxSounds = 64;
  static constexpr std::uint32_t kMaxSources = 32;

  // Opens the default audio device and makes the context current. Fails
  // with already_initialized / device_open_failed.
  [[nodiscard]] ws::core::Status init();

  // Tears the device down and drops every handle. Idempotent and always
  // successful - a dead engine is a valid state to shut down, so the
  // destructor runs it too.
  ws::core::Status shutdown();

  // Decodes the wav file and loads it; load_wav's error surfaces unchanged.
  [[nodiscard]] ws::core::Result<SoundHandle> load_sound(std::string_view wav_path);
  [[nodiscard]] ws::core::Result<SoundHandle> load_sound(const SoundDesc& desc);

  // Frees the sound. Sources still playing it are stopped and their slots
  // reclaimed; the sound handle goes stale.
  [[nodiscard]] ws::core::Status unload_sound(SoundHandle sound);

  // Starts a one-shot playback and returns its source. gain is linear
  // [0, 1+]; positions start at the listener, spatialize with
  // set_position. The slot is held until stop() or a finished-playback
  // update() frees it - always eventually call one of the two.
  [[nodiscard]] ws::core::Result<SourceHandle> play(SoundHandle sound,
                                                    float gain = 1.0f);

  // Stops playback and frees the slot. Stopping a finished source is the
  // same operation: the handle goes stale either way.
  [[nodiscard]] ws::core::Status stop(SourceHandle source);

  [[nodiscard]] ws::core::Status set_position(SourceHandle source, float x,
                                              float y, float z);
  [[nodiscard]] ws::core::Status set_gain(SourceHandle source, float gain);

  // The one bool API: anything not a live, currently-playing source reads
  // false - pre-init, stale, stopped, finished alike.
  [[nodiscard]] bool is_playing(SourceHandle source) const;

  // Reaps sources whose playback finished on its own: OpenAL moves them to
  // stopped, this frees the slot and retires the handle. Call from the main
  // loop (the wavplayer app's wait loop is the template).
  [[nodiscard]] ws::core::Status update();

  // Listener placement - plain floats on purpose, ws::math stays out of the
  // public surface. Defaults: origin, facing -z, up +y.
  [[nodiscard]] ws::core::Status set_listener_position(float x, float y, float z);
  [[nodiscard]] ws::core::Status set_listener_orientation(float forward_x,
                                                          float forward_y,
                                                          float forward_z,
                                                          float up_x, float up_y,
                                                          float up_z);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace ws::audio
