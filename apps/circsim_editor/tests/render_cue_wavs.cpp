// Renders circsim_editor's four sound cues to WAV files (the audible
// proof of the synth) and round-trips each through ws::audio's own
// load_wav - the same decoder the engine plays through. Writes only to
// the paths given as arguments.
#include <cstdio>
#include <string_view>
#include <vector>

#include <ws/audio/wav.hpp>

#include "sound.hpp"

namespace {

struct Named {
    editor::SoundCue cue;
    const char* file;
};

int run(const std::string& out_dir) {
    const Named all[] = {
        {editor::SoundCue::edit, "circsim-sound-edit-tick.wav"},
        {editor::SoundCue::run_done, "circsim-sound-run-chime.wav"},
        {editor::SoundCue::run_failed, "circsim-sound-run-buzz.wav"},
        {editor::SoundCue::error, "circsim-sound-error-ding.wav"},
    };
    int failures = 0;
    for (const Named& n : all) {
        const std::vector<char> bytes = editor::cue_to_wav_bytes(n.cue);
        const std::string path = out_dir + "/" + n.file;
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            const std::size_t wrote =
                std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
            if (wrote != bytes.size()) {
                std::printf("FAIL write %s\n", path.c_str());
                ++failures;
                continue;
            }
        } else {
            std::printf("FAIL open %s\n", path.c_str());
            ++failures;
            continue;
        }
        // The round-trip: the audio lib's own decoder reads the file
        // back - sample count and rate must match the synth's contract.
        const auto wav = ws::audio::load_wav(path);
        const editor::CueData pcm = editor::synth_cue(n.cue);
        if (!wav.has_value()) {
            std::printf("FAIL decode %s\n", path.c_str());
            ++failures;
            continue;
        }
        const bool ok = wav->channels == 1u &&
                        wav->sample_rate == pcm.sample_rate &&
                        wav->frame_count() == pcm.frame_count();
        std::printf("%s %zu bytes, %u frames @ %u Hz, decode %s\n",
                    ok ? "OK " : "BAD", bytes.size(),
                    wav->frame_count(), wav->sample_rate,
                    ok ? "matches" : "MISMATCH");
        if (!ok) {
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: render_cue_wavs <out-dir>\n");
        return 2;
    }
    return run(std::string_view{argv[1]} == "-" ? std::string{"."}
                                                : std::string{argv[1]});
}
