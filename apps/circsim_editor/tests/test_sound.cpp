// The sound effects' own gate, headless by construction: no device is
// ever opened (the audio lib's engine belongs to the app's SoundHost
// alone; these tests own the synth bytes and the cooldown policy).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "../src/sound.hpp"
#include "../src/editor.hpp"
#include "../src/document.hpp"

#include <ws/settings/filesystem.hpp>

namespace gui = ws::ui::gui;

namespace {

using editor::CueData;
using editor::SoundCue;

// The whole family's loudness facts, over the synthesized bytes.
struct CueStats {
    double peak = 0.0;          // max |sample| / 32768
    double rms = 0.0;           // the energy
    bool starts_quiet = false;  // first window below 1% peak
    bool ends_silent = false;   // last sample ~0 (the click-free proof)
    std::size_t frames = 0;
};

[[nodiscard]] CueStats stats_of(const CueData& c) {
    CueStats s;
    s.frames = c.samples.size();
    double sum_sq = 0.0;
    for (std::int16_t v : c.samples) {
        const double a =
            std::abs(static_cast<double>(v)) / 32'768.0;
        s.peak = std::max(s.peak, a);
        sum_sq += a * a;
    }
    s.rms = std::sqrt(sum_sq / static_cast<double>(
                                   std::max<std::size_t>(s.frames, 1u)));
    if (!c.samples.empty()) {
        s.starts_quiet = std::abs(
                             static_cast<double>(c.samples.front())) <
                         0.01 * 32'768.0;
        s.ends_silent =
            std::abs(static_cast<double>(c.samples.back())) <
            0.01 * 32'768.0;
    }
    return s;
}

}  // namespace

// --- the synth: four distinct, bounded, click-free cues ----------------------

TEST_CASE("the four cues synthesize nonempty mono PCM at one rate") {
    for (int i = 0; i <= static_cast<int>(SoundCue::error); ++i) {
        const CueData c = editor::synth_cue(static_cast<SoundCue>(i));
        CHECK(c.sample_rate == 44'100u);
        CHECK(c.frame_count() > 0u);
        CHECK(c.frame_count() < 44'100u);  // every cue under a second
        INFO("cue ", i, " frames=", c.frame_count());
    }
}

TEST_CASE("every cue starts quiet and ends silent (click-free)") {
    // The hann window's contract: 0 amplitude at both ends. A click is
    // a step the cone must rest through - the proof is the ENDS, not
    // the middle.
    for (int i = 0; i <= static_cast<int>(SoundCue::error); ++i) {
        const CueData c = editor::synth_cue(static_cast<SoundCue>(i));
        const CueStats s = stats_of(c);
        CHECK(s.starts_quiet);
        CHECK(s.ends_silent);
        INFO("cue ", i);
    }
}

TEST_CASE("every cue is bounded below clipping and carries energy") {
    for (int i = 0; i <= static_cast<int>(SoundCue::error); ++i) {
        const CueData c = editor::synth_cue(static_cast<SoundCue>(i));
        const CueStats s = stats_of(c);
        CHECK(s.peak < 0.999);      // headroom held
        CHECK(s.peak > 0.05);        // actually audible material
        CHECK(s.rms > 0.02);        // not one lonely spike
        INFO("cue ", i, " peak=", s.peak, " rms=", s.rms);
    }
}

TEST_CASE("the four cues are distinct material, not four copies") {
    // Same length family members with different spectral content: the
    // RMS and peak fingerprint separates them pairwise. The two-tone
    // chime and the buzz differ the most; the check is pairwise
    // inequality of (peak, rms, frames) triples.
    struct Fingerprint {
        double peak, rms;
        std::size_t frames;
    };
    Fingerprint fp[4];
    for (int i = 0; i < 4; ++i) {
        const CueStats s =
            stats_of(editor::synth_cue(static_cast<SoundCue>(i)));
        fp[i] = {s.peak, s.rms, s.frames};
    }
    std::size_t same = 0;
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            const bool identical = std::abs(fp[a].peak - fp[b].peak) <
                                        1e-9 &&
                                    std::abs(fp[a].rms - fp[b].rms) < 1e-9 &&
                                    fp[a].frames == fp[b].frames;
            if (identical) {
                ++same;
            }
        }
    }
    CHECK(same == 0);
}

TEST_CASE("the edit tick is the shortest cue and the chime the longest") {
    // The cadence contract: an edit tick must be a blip (a burst of
    // drags must not sound like an alarm) and the run's chime is the
    // one cue that may breathe.
    const std::size_t tick =
        editor::synth_cue(SoundCue::edit).frame_count();
    const std::size_t chime =
        editor::synth_cue(SoundCue::run_done).frame_count();
    const std::size_t buzz =
        editor::synth_cue(SoundCue::run_failed).frame_count();
    const std::size_t ding =
        editor::synth_cue(SoundCue::error).frame_count();
    CHECK(tick < chime);
    CHECK(tick < ding);
    CHECK(tick < buzz);
}

TEST_CASE("the edit tick is the quietest cue") {
    // A quiet tick, not a beep - the amplitude knob is the contract.
    const double tick_rms =
        stats_of(editor::synth_cue(SoundCue::edit)).rms;
    for (int i = 1; i < 4; ++i) {
        const double other = stats_of(
                                 editor::synth_cue(
                                     static_cast<SoundCue>(i)))
                                 .rms;
        CHECK(tick_rms < other);
    }
}

// --- the WAV proof path -------------------------------------------------------

TEST_CASE("cue_to_wav_bytes writes a canonical 44-byte mono header") {
    for (int i = 0; i <= static_cast<int>(SoundCue::error); ++i) {
        const CueData c = editor::synth_cue(static_cast<SoundCue>(i));
        const std::vector<char> wav =
            editor::cue_to_wav_bytes(static_cast<SoundCue>(i));
        REQUIRE(wav.size() == 44u + 2u * c.frame_count());
        CHECK(std::string_view{wav.data(), 4} == "RIFF");
        CHECK(std::string_view{wav.data() + 8, 4} == "WAVE");
        CHECK(std::string_view{wav.data() + 36, 4} == "data");
        // fmt: PCM, mono, 44.1k, 16-bit - the little-endian reads.
        const auto u16 = [&](std::size_t off) {
            return static_cast<std::uint16_t>(
                static_cast<std::uint8_t>(wav[off]) |
                (static_cast<std::uint8_t>(wav[off + 1]) << 8));
        };
        CHECK(u16(20) == 1u);
        CHECK(u16(22) == 1u);
        CHECK(u16(34) == 16u);
        INFO("cue ", i);
    }
}

TEST_CASE("the wav payload is the synth's own samples, byte for byte") {
    const CueData c = editor::synth_cue(SoundCue::run_done);
    const std::vector<char> wav = editor::cue_to_wav_bytes(SoundCue::run_done);
    for (std::size_t f = 0; f < c.samples.size(); ++f) {
        const auto lo = static_cast<std::uint8_t>(wav[44 + 2 * f]);
        const auto hi = static_cast<std::uint8_t>(wav[44 + 2 * f + 1]);
        const auto back = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(lo | (hi << 8)));
        CHECK(back == c.samples[f]);
    }
}

// --- the policy: cooldowns over an injected clock -----------------------------

TEST_CASE("sound off: no cue ever fires") {
    editor::Editor ed;
    CHECK_FALSE(ed.sound.enabled);
    ed.sound.now_s = 1.0;
    CHECK_FALSE(ed.sound.edited());
    CHECK_FALSE(ed.sound.run_done());
    CHECK_FALSE(ed.sound.run_failed());
    CHECK_FALSE(ed.sound.error());
}

TEST_CASE("a cue fires once then swallows inside its cooldown") {
    editor::Editor ed;
    ed.sound.enabled = true;
    ed.sound.now_s = 10.0;
    CHECK(ed.sound.edited());           // the tick lands
    CHECK_FALSE(ed.sound.edited());     // the burst member dies
    ed.sound.now_s = 10.11;
    CHECK_FALSE(ed.sound.edited());     // still inside 0.12s
    ed.sound.now_s = 10.13;
    CHECK(ed.sound.edited());           // the window passed
}

TEST_CASE("each cue's cooldown is its own - no cross-muting") {
    editor::Editor ed;
    ed.sound.enabled = true;
    ed.sound.now_s = 5.0;
    CHECK(ed.sound.edited());
    // The chime is NOT muted by the tick's window: the moments are
    // different classes, a converged run after a typed burst still
    // chimes.
    CHECK(ed.sound.run_done());
    CHECK(ed.sound.run_failed());
    CHECK(ed.sound.error());
}

TEST_CASE("the chime's cooldown is long enough for a converging loop") {
    editor::Editor ed;
    ed.sound.enabled = true;
    ed.sound.now_s = 0.0;
    CHECK(ed.sound.run_done());
    // A fast loop (settle 0.1s) delivering at 0.2s, 0.3s: only the
    // second survives past 0.8s - a chime per second, not per run.
    ed.sound.now_s = 0.2;
    CHECK_FALSE(ed.sound.run_done());
    ed.sound.now_s = 0.85;
    CHECK(ed.sound.run_done());
}

TEST_CASE("document_changed resets the cooldowns (a fresh document's first sounds)") {
    ws::settings::StdFileSystem fs;
    editor::SchematicDocument doc{std::string{}, fs};
    editor::Editor ed;
    ed.doc = &doc;
    ed.sound.enabled = true;
    ed.sound.now_s = 3.0;
    CHECK(ed.sound.edited());
    CHECK_FALSE(ed.sound.edited());
    ed.document_changed();
    // The clock rewinds too (the reset's whole contract), so the first
    // fire after the switch answers at now_s 0 - never swallowed by the
    // old document's window.
    CHECK(ed.sound.now_s == 0.0);
    CHECK(ed.sound.edited());
}

TEST_CASE("the toast's error cue rides the severity mapping") {
    // The toast path is the only cue the EDITOR fires on its own (the
    // app fires edit/run pairs at their own sites). An error toast
    // sounds; warnings and info never do - a ding per advisory is a
    // nag, and info is the toast's own visual.
    ws::settings::StdFileSystem fs;
    editor::SchematicDocument doc{std::string{}, fs};
    editor::Editor ed;
    ed.doc = &doc;
    // The real store (the same type the app installs - push/count is
    // its whole surface; the bytes are Editor's own stable slots).
    gui::Toasts list;
    ed.toasts = &list;
    // The sound seam, counting cues.
    std::vector<int> cues;
    ed.play_sound = [&cues](int cue) { cues.push_back(cue); };
    ed.sound.enabled = true;

    ed.toast("boom", gui::ToastSeverity::error);
    CHECK(list.count() == 1);
    REQUIRE(cues.size() == 1u);  // the ding
    CHECK(cues.front() ==
          static_cast<int>(editor::SoundCue::error));

    // A SECOND distinct error text (the dedupe cannot kill it) still
    // inside the cue's 1.0s cooldown is swallowed by the cooldown -
    // the toast itself still lands on screen.
    ed.toast("another failure", gui::ToastSeverity::error);
    CHECK(list.count() == 2);
    CHECK(cues.size() == 1u);  // the ding does not repeat

    // Warning toasts never sound at all.
    cues.clear();
    ed.toast("careful", gui::ToastSeverity::warning);
    CHECK(list.count() == 3);
    CHECK(cues.empty());
    ed.toast("fyi", gui::ToastSeverity::info);
    CHECK(cues.empty());

    // ...and with sound disabled the error toast is silent too.
    ed.sound.enabled = false;
    cues.clear();
    ed.toast("quiet boom", gui::ToastSeverity::error);
    CHECK(cues.empty());
    CHECK(list.count() == 5);  // error, error, warning, info, error
}

// --- the cadence, end to end over the injected clock -------------------------

TEST_CASE("a typed burst sounds one tick, a delivered run one chime") {
    // The whole point of the policy, as one scripted session: a burst
    // of edits (drag commits at frame cadence - 60Hz frames land
    // 16ms apart), then the run the loop fires converges, then the
    // NEXT burst after the chime's window. Two ticks, one chime, zero
    // noise between.
    editor::Editor ed;
    ed.sound.enabled = true;
    std::vector<int> cues;
    ed.play_sound = [&cues](int c) { cues.push_back(c); };

    // The burst: 8 edits over 120ms (frame-cadence commits).
    for (int f = 0; f < 8; ++f) {
        ed.sound.now_s = 1.0 + 0.016 * f;
        if (ed.sound.edited()) {
            ed.play_sound(static_cast<int>(SoundCue::edit));
        }
    }
    REQUIRE(cues.size() == 1u);  // the burst collapsed

    // The run lands at 1.4s: the chime.
    ed.sound.now_s = 1.4;
    if (ed.sound.run_done()) {
        ed.play_sound(static_cast<int>(SoundCue::run_done));
    }
    CHECK(cues.size() == 2u);
    CHECK(cues.back() == static_cast<int>(SoundCue::run_done));

    // A converged loop re-delivering at 0.1s cadence: the next chime
    // waits out the 0.8s window - run after run reads as one chime
    // per second. (r=8 lands at 1.4+0.8: the 0.8s window EXPIRED, the
    // chime answers - the boundary is `now - last < cooldown`, so the
    // first chime past the window is exactly the one that waits 0.8s.)
    for (int r = 1; r <= 8; ++r) {
        ed.sound.now_s = 1.5 + 0.1 * r;
        if (ed.sound.run_done()) {
            ed.play_sound(static_cast<int>(SoundCue::run_done));
        }
    }
    CHECK(cues.size() == 3u);  // one more chime across 0.8s of runs
}
