#include <ws/audio/wav.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dr_wav.h>  // private: never in a public header

namespace ws::audio {

ws::core::Result<WavSound> load_wav(std::string_view path) {
  const std::string file_path{path};
  std::error_code ec{};
  if (!std::filesystem::exists(file_path, ec))
    return fail(AudioError::file_open_failed, "no such file: " + file_path);

  drwav wav{};
  if (drwav_init_file(&wav, file_path.c_str(), nullptr) == DRWAV_FALSE)
    return fail(AudioError::wav_decode_failed,
                "not a decodable RIFF/WAVE file: " + file_path);
  struct Uninit {
    drwav* wav;
    ~Uninit() { drwav_uninit(wav); }
  } uninit{&wav};

  const auto frames = wav.totalPCMFrameCount;
  const auto channels = static_cast<std::uint32_t>(wav.channels);
  const auto rate = static_cast<std::uint32_t>(wav.sampleRate);
  if (frames == 0 || channels == 0 || rate == 0)
    return fail(AudioError::wav_decode_failed,
                "empty or formatless stream: " + file_path);

  const std::uint64_t total_samples = frames * channels;
  if (total_samples > 0xFFFF'FFFFu)
    return fail(AudioError::wav_too_large,
                "more than 2^32 samples is out of the uint32 surface");

  WavSound out;
  out.channels = channels;
  out.sample_rate = rate;
  out.samples.resize(static_cast<std::size_t>(total_samples));
  const auto read = drwav_read_pcm_frames_s16(&wav, frames, out.samples.data());
  if (read != frames)
    return fail(AudioError::wav_decode_failed,
                "short read while decoding: " + file_path);
  return out;
}

}  // namespace ws::audio
