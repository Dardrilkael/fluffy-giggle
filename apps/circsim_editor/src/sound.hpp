// The editor's sound effects, headless to the last sample: the four
// cues are PROCEDURAL (synthesized PCM at load, like the playground's
// arpeggio - no asset files ship with the app), the policy that gates
// them lives in Editor::Sound (a seam the app installs), and the
// device belongs to the app's SoundHost alone (ws::audio's engine is
// process-global, so the editor NEVER touches it directly - a test
// bench has no device at all and everything still runs).
#pragma once

#include <cstdint>
#include <vector>

#include <ws/audio/engine.hpp>

namespace editor {

// The four cues the editor speaks. The mapping (who fires what) is
// fixed beside the enum so the policy and the host can never disagree
// about which sound a moment gets:
//   edit - every edit commit (an undo push, an undo, a redo) while
//          sound is on; quiet and SHORT (a tick, not a beep - a burst
//          of drags must not read as an alarm).
//   run_done - a run delivered WITH results (the loop's convergence
//          step); the two-tone chime.
//   run_failed - a run delivered WITHOUT results (or the job itself
//          erroring); the low buzz.
//   error - an error toast; the sharp ding. (Warning toasts stay
//          SILENT - most warnings are advisory, a ding per warning is
//          a nag.)
enum class SoundCue : std::uint8_t { edit, run_done, run_failed, error };

// One synthesized cue: 16-bit mono PCM at the app's mix rate, ready
// for SoundDesc. The synth owns the envelope math (click-free by
// construction - every cue ends at a zero crossing and fades), so the
// host's only job is to hand the bytes to the engine.
struct CueData {
    std::vector<std::int16_t> samples;
    std::uint32_t sample_rate = 0;

    [[nodiscard]] std::uint32_t frame_count() const noexcept {
        return static_cast<std::uint32_t>(samples.size());
    }
};

// Synthesizes the named cue. Pure: same cue in, same bytes out, no
// clock, no device, no state - fully unit-testable and the WAV proof
// files in download/ render from exactly this function.
[[nodiscard]] CueData synth_cue(SoundCue cue);

// Renders a cue to a canonical 44-byte-header RIFF/WAVE file (16-bit
// PCM mono). The audible proof the cues are real sounds - also the
// exact bytes load_wav would decode back.
[[nodiscard]] std::vector<char> cue_to_wav_bytes(SoundCue cue);

}  // namespace editor
