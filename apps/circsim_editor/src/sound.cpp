#include "sound.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string_view>

namespace editor {
namespace {

// The whole cue family renders at one rate - the engine's SoundDesc
// carries it, and one rate means the mix math never resamples.
constexpr std::uint32_t kRate = 44'100;

// A raised-cosine (Hann) window over [0, frames): the envelope every
// cue rides. Click-free by construction - it is 0 at both ends (the
// speaker cone starts and rests at its resting position, never a step
// that reads as a click), and it has no corners to ring.
[[nodiscard]] double hann(std::uint32_t i, std::uint32_t frames) {
    const double x = static_cast<double>(i) / static_cast<double>(frames);
    return 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * x));
}

// The decay every cue's TAIL rides after its body: a straight
// exponential fall from 1 to 0 across the fade's span, joined AFTER
// the hann window (the two multiply), so the cue's own end is always
// amplitude 0. Frames past the fade's span are silence.
[[nodiscard]] double tail_decay(std::uint32_t i, std::uint32_t body,
                                std::uint32_t fade) {
    if (i < body) {
        return 1.0;
    }
    const double past = static_cast<double>(i - body) /
                        static_cast<double>(fade);
    if (past >= 1.0) {
        return 0.0;
    }
    return std::exp(-4.0 * past) * (1.0 - past);
}

// The synth's one voice: `partials` harmonics at integral multiples,
// amplitude 1/k (a triangle-ish timbre), windowed and clamped to the
// headroom. `frames` covers the whole cue (body + fade).
[[nodiscard]] std::vector<std::int16_t> tone(double hz,
                                             std::uint32_t body_frames,
                                             std::uint32_t fade_frames,
                                             int partials, double amp) {
    const std::uint32_t frames = body_frames + fade_frames;
    std::vector<std::int16_t> out(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int k = 1; k <= partials; ++k) {
            s += std::sin(2.0 * std::numbers::pi * hz *
                          static_cast<double>(k) *
                          static_cast<double>(i) /
                          static_cast<double>(kRate)) /
                 static_cast<double>(k);
        }
        const double env =
            hann(i, frames) * tail_decay(i, body_frames, fade_frames);
        // Headroom: partials sum to < sum(1/k) ~ H_partials; the amp
        // knob is a FRACTION of full scale (0..1), the int16
        // conversion happens at PCM scale - the audio demo's own
        // 12'000 rule (a v under 1.0 would truncate to a silent 0).
        double v = s * env * amp * 30'000.0;
        if (v > 0.99 * 32'000.0) {
            v = 0.99 * 32'000.0;
        } else if (v < -0.99 * 32'000.0) {
            v = -0.99 * 32'000.0;
        }
        out[i] = static_cast<std::int16_t>(v);
    }
    return out;
}

// Concatenates two tones with the SECOND starting fresh (its own
// envelope): the two-tone chime's body. The seam between the two
// rides the first tone's hann-to-zero, so it stays click-free.
[[nodiscard]] std::vector<std::int16_t> two_tone(
    double hz_a, std::uint32_t body_a, double hz_b, std::uint32_t body_b,
    std::uint32_t fade) {
    std::vector<std::int16_t> a =
        tone(hz_a, body_a, fade, 3, 0.30);
    std::vector<std::int16_t> b =
        tone(hz_b, body_b, fade, 3, 0.30);
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// The buzz's carrier: square-ish (odd partials only, the clarinet's
// own series) at a LOW pitch. Deliberately unpleasant-lite - a buzz
// must be audible across the room, not pleasant up close.
[[nodiscard]] std::vector<std::int16_t> buzz(double hz,
                                             std::uint32_t body,
                                             std::uint32_t fade) {
    // Odd partials only, amplitude 1/k^2 (softer than the tone's).
    const std::uint32_t frames = body + fade;
    std::vector<std::int16_t> out(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        double s = 0.0;
        for (int k = 1; k <= 7; k += 2) {
            s += std::sin(2.0 * std::numbers::pi * hz *
                          static_cast<double>(k) *
                          static_cast<double>(i) /
                          static_cast<double>(kRate)) /
                 (static_cast<double>(k) * static_cast<double>(k));
        }
        const double env =
            hann(i, frames) * tail_decay(i, body, fade);
        out[i] = static_cast<std::int16_t>(
            std::clamp(s * env * 0.55 * 28'000.0, -0.99 * 32'000.0,
                       0.99 * 32'000.0));
    }
    return out;
}

// A canonical 44-byte-header RIFF/WAVE writer for 16-bit mono PCM -
// the same shape the audio lib's own tests build, so load_wav (dr_wav
// under the pimpl) round-trips it.
void push_u32(std::vector<char>& b, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) {
        b.push_back(static_cast<char>((v >> s) & 0xFF));
    }
}
void push_u16(std::vector<char>& b, std::uint16_t v) {
    b.push_back(static_cast<char>(v & 0xFF));
    b.push_back(static_cast<char>(v >> 8));
}

}  // namespace

CueData synth_cue(SoundCue cue) {
    switch (cue) {
        case SoundCue::edit: {
            // 60ms of a single 2.2kHz-ish tick, hard-attacked
            // (hann's rise is 4ms at this length - reads as a tick,
            // not a swell), tiny fade. The quietest of the four: a
            // burst of drags plays a burst of ticks, never an alarm.
            return {tone(2'200.0, kRate * 48 / 1'000,
                          kRate * 12 / 1'000, 1, 0.12),
                    kRate};
        }
        case SoundCue::run_done: {
            // The two-tone chime: a fifth (660 -> 990) over ~230ms.
            // "It converged and the numbers are on screen" - the
            // loop's own reward, pitched up.
            return {two_tone(660.0, kRate * 80 / 1'000, 990.0,
                             kRate * 110 / 1'000, kRate * 40 / 1'000),
                    kRate};
        }
        case SoundCue::run_failed: {
            // The low buzz: ~140ms at 110Hz, odd partials. Flat,
            // downward feel without a pitch bend (a bend is melodic -
            // failure should be a grunt).
            return {buzz(110.0, kRate * 100 / 1'000,
                         kRate * 40 / 1'000),
                    kRate};
        }
        case SoundCue::error: {
            // The sharp ding: 90ms at 1.5kHz with a longer natural
            // tail, one partial only (a sine - clean, urgent, short).
            return {tone(1'500.0, kRate * 70 / 1'000,
                         kRate * 60 / 1'000, 1, 0.35),
                    kRate};
        }
    }
    return {};  // unreachable, but the compiler wants it
}

std::vector<char> cue_to_wav_bytes(SoundCue cue) {
    const CueData pcm = synth_cue(cue);
    const std::uint32_t frames = pcm.frame_count();
    const std::uint32_t data_bytes = frames * 2u;

    std::vector<char> b;
    b.reserve(44u + data_bytes);
    for (const char c : std::string_view{"RIFF"}) {
        b.push_back(c);
    }
    push_u32(b, 36u + data_bytes);
    for (const char c : std::string_view{"WAVE"}) {
        b.push_back(c);
    }
    for (const char c : std::string_view{"fmt "}) {
        b.push_back(c);
    }
    push_u32(b, 16u);                  // fmt chunk size
    push_u16(b, 1u);                   // PCM
    push_u16(b, 1u);                    // mono
    push_u32(b, pcm.sample_rate);
    push_u32(b, pcm.sample_rate * 2u);  // byte rate
    push_u16(b, 2u);                    // block align
    push_u16(b, 16u);                   // bits per sample
    for (const char c : std::string_view{"data"}) {
        b.push_back(c);
    }
    push_u32(b, data_bytes);
    for (std::int16_t s : pcm.samples) {
        push_u16(b, static_cast<std::uint16_t>(s));
    }
    return b;
}

}  // namespace editor
