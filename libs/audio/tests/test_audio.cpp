#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ws/audio/engine.hpp>
#include <ws/audio/errors.hpp>
#include <ws/audio/wav.hpp>

namespace {

// ---- fixture building -------------------------------------------------------
// Tests build their own tiny RIFF/WAVE files in the temp dir instead of
// committing binary fixtures: the bytes stay under test control and nothing
// gets written next to the repo (a throwaway audio experiment once dropped
// its fixtures in the repo root - do not repeat that).

void append_u16(std::vector<char>& bytes, std::uint16_t v) {
  bytes.push_back(static_cast<char>(v & 0xFF));
  bytes.push_back(static_cast<char>(v >> 8));
}

void append_u32(std::vector<char>& bytes, std::uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((v >> shift) & 0xFF));
}

// A 16-bit PCM sine packed into a canonical 44-byte-header wav.
std::vector<char> sine_wav_bytes(std::uint32_t frames, std::uint32_t channels,
                                 std::uint32_t rate, double hz) {
  const auto block_align = static_cast<std::uint32_t>(2 * channels);
  const auto data_bytes = frames * block_align;
  std::vector<char> bytes;
  bytes.reserve(44 + data_bytes);
  for (const char c : std::string_view{"RIFF"})
    bytes.push_back(c);
  append_u32(bytes, 36 + data_bytes);
  for (const char c : std::string_view{"WAVE"})
    bytes.push_back(c);
  for (const char c : std::string_view{"fmt "})
    bytes.push_back(c);
  append_u32(bytes, 16);  // fmt chunk size
  append_u16(bytes, 1);   // PCM
  append_u16(bytes, static_cast<std::uint16_t>(channels));
  append_u32(bytes, rate);
  append_u32(bytes, rate * block_align);
  append_u16(bytes, static_cast<std::uint16_t>(block_align));
  append_u16(bytes, 16);  // bits per sample
  for (const char c : std::string_view{"data"})
    bytes.push_back(c);
  append_u32(bytes, data_bytes);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    const double s = std::sin(2.0 * std::numbers::pi * hz * t);
    const auto pcm = static_cast<std::int16_t>(s * 8000.0);
    for (std::uint32_t c = 0; c < channels; ++c)
      append_u16(bytes, static_cast<std::uint16_t>(pcm));
  }
  return bytes;
}

std::filesystem::path write_fixture(const std::string& name,
                                    const std::vector<char>& bytes) {
  const auto dir = std::filesystem::temp_directory_path() / "ws_audio_tests";
  std::filesystem::create_directories(dir);
  const auto path = dir / name;
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

}  // namespace

TEST_CASE("load_wav decodes a mono 8 kHz sine") {
  const auto path = write_fixture("mono.wav", sine_wav_bytes(400, 1, 8000, 440.0));
  const auto wav = ws::audio::load_wav(path.string());
  REQUIRE(wav.has_value());
  CHECK(wav->channels == 1);
  CHECK(wav->sample_rate == 8000);
  CHECK(wav->frame_count() == 400);
  REQUIRE(wav->samples.size() == 400);
  // "some sample is nonzero" rather than a fixed index: 440 Hz at 8 kHz
  // crosses zero exactly at frame 100 (440 * 100/8000 = 5.5 cycles), so an
  // index-specific check silently depends on the frequency/rate pairing.
  CHECK(std::any_of(wav->samples.begin(), wav->samples.end(),
                    [](std::int16_t s) { return s != 0; }));
}

TEST_CASE("load_wav decodes a stereo wav with interleaved samples") {
  const auto path = write_fixture("stereo.wav", sine_wav_bytes(200, 2, 8000, 440.0));
  const auto wav = ws::audio::load_wav(path.string());
  REQUIRE(wav.has_value());
  CHECK(wav->channels == 2);
  CHECK(wav->sample_rate == 8000);
  CHECK(wav->frame_count() == 200);
  CHECK(wav->samples.size() == 400);
}

TEST_CASE("load_wav rejects a file that is not RIFF/WAVE") {
  const std::string_view joke = "this is not a wav file at all";
  const std::vector<char> bytes{joke.begin(), joke.end()};
  const auto path = write_fixture("garbage.wav", bytes);
  const auto wav = ws::audio::load_wav(path.string());
  REQUIRE(!wav.has_value());
  CHECK(wav.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::wav_decode_failed));
  CHECK(std::string_view{wav.error().category} == "audio");
}

TEST_CASE("load_wav rejects a missing file") {
  const auto wav = ws::audio::load_wav("definitely/not/here.wav");
  REQUIRE(!wav.has_value());
  CHECK(wav.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::file_open_failed));
}

TEST_CASE("load_wav rejects a truncated header") {
  std::vector<char> bytes{'R', 'I', 'F', 'F'};
  const auto path = write_fixture("truncated.wav", bytes);
  const auto wav = ws::audio::load_wav(path.string());
  REQUIRE(!wav.has_value());
  CHECK(wav.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::wav_decode_failed));
}

// ---- engine ------------------------------------------------------------------
// Device playback is exercised for real (the default output device opens),
// but every test plays at gain 0: the state machine under test is
// independent of what reaches the speakers, and a suite that beeps is a
// suite nobody runs twice.

std::vector<std::int16_t> sine_pcm(std::uint32_t frames, std::uint32_t rate,
                                   double hz) {
  std::vector<std::int16_t> out(frames);
  for (std::uint32_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / rate;
    out[i] = static_cast<std::int16_t>(std::sin(2.0 * std::numbers::pi * hz * t) *
                                       8000.0);
  }
  return out;
}

TEST_CASE("engine rejects calls before init") {
  ws::audio::AudioEngine engine;
  auto samples = sine_pcm(100, 8000, 440.0);
  const ws::audio::SoundDesc desc{samples.data(), 100, 1, 8000};
  const auto not_init =
      static_cast<std::int32_t>(ws::audio::AudioError::not_initialized);

  CHECK(engine.load_sound(desc).error().code == not_init);
  CHECK(engine.play(ws::audio::SoundHandle{1}).error().code == not_init);
  CHECK(engine.stop(ws::audio::SourceHandle{1}).error().code == not_init);
  CHECK(engine.unload_sound(ws::audio::SoundHandle{1}).error().code == not_init);
  CHECK(engine.update().error().code == not_init);
  CHECK(engine.set_position(ws::audio::SourceHandle{1}, 0.0f, 0.0f, 0.0f)
            .error()
            .code == not_init);
  CHECK(engine.set_gain(ws::audio::SourceHandle{1}, 1.0f).error().code ==
        not_init);
  CHECK(engine.set_listener_position(0.0f, 0.0f, 0.0f).error().code == not_init);
  CHECK(engine.set_listener_orientation(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f)
            .error()
            .code == not_init);
  CHECK(!engine.is_playing(ws::audio::SourceHandle{1}));  // bool API: no error
}

TEST_CASE("engine init is single-shot and shutdown is idempotent") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());

  const auto again = engine.init();
  REQUIRE(!again.has_value());
  CHECK(again.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::already_initialized));

  CHECK(engine.shutdown().has_value());
  CHECK(engine.shutdown().has_value());  // dead engines shut down fine

  CHECK(engine.init().has_value());  // reusable after shutdown
}

TEST_CASE("load_sound issues distinct valid handles") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());

  const auto path = write_fixture("engine.wav", sine_wav_bytes(100, 1, 8000, 440.0));
  const auto from_file = engine.load_sound(path.string());
  REQUIRE(from_file.has_value());
  CHECK(from_file->valid());

  auto samples = sine_pcm(100, 8000, 440.0);
  const ws::audio::SoundDesc desc{samples.data(), 100, 1, 8000};
  const auto from_memory = engine.load_sound(desc);
  REQUIRE(from_memory.has_value());
  CHECK(from_memory->valid());

  CHECK(from_memory->value > from_file->value);  // monotonic, never recycled
}

TEST_CASE("play starts a source and stop retires its handle") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(8000, 8000, 440.0);  // 1 s of tone
  const ws::audio::SoundDesc desc{samples.data(), 8000, 1, 8000};
  const auto sound = engine.load_sound(desc);
  REQUIRE(sound.has_value());

  const auto source = engine.play(*sound, 0.0f);
  REQUIRE(source.has_value());
  CHECK(engine.is_playing(*source));

  CHECK(engine.stop(*source).has_value());
  CHECK(!engine.is_playing(*source));  // slot gone with the handle

  const auto stale = engine.stop(*source);
  REQUIRE(!stale.has_value());
  CHECK(stale.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::invalid_handle));
}

TEST_CASE("update reaps sources that finished on their own") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(400, 8000, 440.0);  // 50 ms of tone
  const ws::audio::SoundDesc desc{samples.data(), 400, 1, 8000};
  const auto sound = engine.load_sound(desc);
  REQUIRE(sound.has_value());
  const auto source = engine.play(*sound, 0.0f);
  REQUIRE(source.has_value());
  CHECK(engine.is_playing(*source));

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  CHECK(!engine.is_playing(*source));  // the mixer reached the end

  CHECK(engine.update().has_value());  // frees the slot and the handle

  const auto stale = engine.stop(*source);
  REQUIRE(!stale.has_value());
  CHECK(stale.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::invalid_handle));
}

TEST_CASE("unload_sound stops its sources and retires the sound") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(8000, 8000, 440.0);
  const ws::audio::SoundDesc desc{samples.data(), 8000, 1, 8000};
  const auto sound = engine.load_sound(desc);
  REQUIRE(sound.has_value());
  const auto source = engine.play(*sound, 0.0f);
  REQUIRE(source.has_value());
  CHECK(engine.is_playing(*source));

  CHECK(engine.unload_sound(*sound).has_value());
  CHECK(!engine.is_playing(*source));  // the source went with the sound

  const auto replay = engine.play(*sound);
  REQUIRE(!replay.has_value());
  CHECK(replay.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::invalid_handle));
}

TEST_CASE("source and listener setters take live handles only") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(8000, 8000, 440.0);
  const ws::audio::SoundDesc desc{samples.data(), 8000, 1, 8000};
  const auto sound = engine.load_sound(desc);
  REQUIRE(sound.has_value());
  const auto source = engine.play(*sound, 0.0f);
  REQUIRE(source.has_value());

  CHECK(engine.set_position(*source, 1.0f, 2.0f, 3.0f).has_value());
  CHECK(engine.set_gain(*source, 0.5f).has_value());
  CHECK(engine.set_listener_position(0.0f, 0.0f, 0.0f).has_value());
  CHECK(engine.set_listener_orientation(0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f)
            .has_value());

  CHECK(engine.stop(*source).has_value());
  const auto stale = engine.set_position(*source, 0.0f, 0.0f, 0.0f);
  REQUIRE(!stale.has_value());
  CHECK(stale.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::invalid_handle));

  const ws::audio::SourceHandle bogus{999};
  CHECK(engine.set_gain(bogus, 1.0f).error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::invalid_handle));
}

TEST_CASE("load_sound validates the descriptor") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(100, 8000, 440.0);
  const auto bad =
      static_cast<std::int32_t>(ws::audio::AudioError::invalid_sound_data);

  CHECK(engine.load_sound(ws::audio::SoundDesc{nullptr, 100, 1, 8000})
            .error()
            .code == bad);
  CHECK(engine.load_sound(ws::audio::SoundDesc{samples.data(), 0, 1, 8000})
            .error()
            .code == bad);
  CHECK(engine.load_sound(ws::audio::SoundDesc{samples.data(), 100, 3, 8000})
            .error()
            .code == bad);
  CHECK(engine.load_sound(ws::audio::SoundDesc{samples.data(), 100, 1, 0})
            .error()
            .code == bad);
}

TEST_CASE("engine enforces its slot caps") {
  ws::audio::AudioEngine engine;
  REQUIRE(engine.init().has_value());
  auto samples = sine_pcm(100, 8000, 440.0);
  const ws::audio::SoundDesc desc{samples.data(), 100, 1, 8000};

  std::vector<ws::audio::SoundHandle> sounds;
  for (std::uint32_t i = 0; i < ws::audio::AudioEngine::kMaxSounds; ++i) {
    const auto sound = engine.load_sound(desc);
    REQUIRE(sound.has_value());
    sounds.push_back(*sound);
  }
  const auto extra_sound = engine.load_sound(desc);
  REQUIRE(!extra_sound.has_value());
  CHECK(extra_sound.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::too_many_sounds));

  // 12.5 ms tones: they finish almost instantly, but only update() frees
  // slots - so the map fills and the cap is what turns the next play away.
  std::vector<ws::audio::SourceHandle> sources;
  for (std::uint32_t i = 0; i < ws::audio::AudioEngine::kMaxSources; ++i) {
    const auto source = engine.play(sounds.front(), 0.0f);
    REQUIRE(source.has_value());
    sources.push_back(*source);
  }
  const auto extra_source = engine.play(sounds.front(), 0.0f);
  REQUIRE(!extra_source.has_value());
  CHECK(extra_source.error().code ==
        static_cast<std::int32_t>(ws::audio::AudioError::too_many_sources));
}
