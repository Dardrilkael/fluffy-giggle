#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

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
