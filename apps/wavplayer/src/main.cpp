// apps/wavplayer - the headless ws::audio example. Synthesizes a little
// C-major arpeggio, plays it through the real audio device and exits when
// the last sample is consumed. Pass a wav path as argv[1] to play that
// instead. No window, no game loop - just ws::core logs and the speakers.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>
#include <vector>

#include <ws/audio/engine.hpp>
#include <ws/core/log.hpp>

namespace {

constexpr std::uint32_t kRate = 44'100;  // Hz
constexpr std::uint32_t kNoteFrames = kRate * 23 / 100;  // 0.23 s per note

// C-major arpeggio, up and back down: C5 E5 G5 C6 G5 E5 C5.
constexpr double kMelody[] = {523.25, 659.25, 783.99, 1046.50,
                              783.99, 659.25, 523.25};
constexpr auto kNoteCount = sizeof(kMelody) / sizeof(kMelody[0]);

// Each note decays linearly to silence, so the arpeggio is legible without
// gaps between notes.
std::vector<std::int16_t> synth_melody() {
  std::vector<std::int16_t> out(kNoteCount * kNoteFrames);
  for (std::size_t note = 0; note < kNoteCount; ++note) {
    const double hz = kMelody[note];
    for (std::uint32_t i = 0; i < kNoteFrames; ++i) {
      const double t = static_cast<double>(i) / kRate;
      const double decay =
          1.0 - static_cast<double>(i) / kNoteFrames;
      out[note * kNoteFrames + i] = static_cast<std::int16_t>(
          std::sin(2.0 * std::numbers::pi * hz * t) * decay * 12'000.0);
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  ws::audio::AudioEngine engine;

  if (auto init = engine.init(); !init) {
    ws::core::log_error(init.error());
    return 1;
  }
  ws::core::info("wavplayer: audio device open");

  // argv[1] plays that file instead of the built-in melody.
  ws::audio::SoundHandle sound{};
  if (argc > 1) {
    auto loaded = engine.load_sound(argv[1]);
    if (!loaded) {
      ws::core::log_error(loaded.error());
      return 1;
    }
    sound = *loaded;
    ws::core::info("wavplayer: loaded {}", argv[1]);
  } else {
    auto samples = synth_melody();
    const ws::audio::SoundDesc desc{samples.data(),
                                    static_cast<std::uint32_t>(samples.size()),
                                    1, kRate};
    auto loaded = engine.load_sound(desc);
    if (!loaded) {
      ws::core::log_error(loaded.error());
      return 1;
    }
    sound = *loaded;
    ws::core::info("wavplayer: synthesized {:.2f} s of C-major arpeggio",
                   static_cast<double>(samples.size()) / kRate);
  }

  const auto source = engine.play(sound);
  if (!source) {
    ws::core::log_error(source.error());
    return 1;
  }
  ws::core::info("wavplayer: playing");

  // Pump update() until the mixer consumed the last sample: it is what
  // frees the source slot once OpenAL reports the sound finished.
  do {
    if (auto updated = engine.update(); !updated) {
      ws::core::log_error(updated.error());
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  } while (engine.is_playing(*source));

  ws::core::info("wavplayer: finished");
  (void)engine.shutdown();
  return 0;
}
