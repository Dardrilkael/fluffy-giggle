#include "audio_demo.hpp"

#include <ws/core/log.hpp>
#include <ws/ui/gui/widgets.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace playground {
namespace {

constexpr std::uint32_t kRate = 44'100;
constexpr std::uint32_t kNoteFrames = kRate * 23 / 100;
constexpr double kMelody[] = {523.25, 659.25, 783.99, 1046.50,
                              783.99, 659.25, 523.25};
constexpr auto kNoteCount = sizeof(kMelody) / sizeof(kMelody[0]);

std::vector<std::int16_t> synth_arpeggio() {
    std::vector<std::int16_t> out(kNoteCount * kNoteFrames);
    for (std::size_t note = 0; note < kNoteCount; ++note) {
        const double hz = kMelody[note];
        for (std::uint32_t i = 0; i < kNoteFrames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double decay = 1.0 - static_cast<double>(i) / kNoteFrames;
            out[note * kNoteFrames + i] = static_cast<std::int16_t>(
                std::sin(2.0 * std::numbers::pi * hz * t) * decay * 12'000.0);
        }
    }
    return out;
}

}  // namespace

void AudioDemo::setup() {
    if (auto init = engine_.init(); !init) {
        ws::core::log_error(init.error());
        return;
    }
    auto samples = synth_arpeggio();
    const ws::audio::SoundDesc desc{
        samples.data(), static_cast<std::uint32_t>(samples.size()), 1, kRate};
    auto sound = engine_.load_sound(desc);
    if (!sound) {
        ws::core::log_error(sound.error());
        return;
    }
    arpeggio_ = *sound;
    ready_ = true;
    ws::core::info("playground: audio device open");
}

void AudioDemo::play() {
    if (!ready_) {
        if (!warned_) {
            warned_ = true;
            ws::core::warn("playground: play pressed, audio is disabled");
        }
        return;
    }
    if (source_.valid()) {
        if (auto stopped = engine_.stop(source_); !stopped) {
            ws::core::log_error(stopped.error());
        }
        source_ = {};
    }
    if (auto source = engine_.play(arpeggio_, gain_); source) {
        source_ = *source;
    } else {
        ws::core::log_error(source.error());
    }
}

void AudioDemo::tick() {
    if (!ready_) return;
    if (auto updated = engine_.update(); !updated) {
        ws::core::log_error(updated.error());
        return;
    }
    if (!engine_.is_playing(source_)) {
        source_ = {};
        return;
    }
    if (auto gain = engine_.set_gain(source_, gain_); !gain) {
        ws::core::log_error(gain.error());
        source_ = {};
        return;
    }
    if (auto pos = engine_.set_position(source_, pan_, 0.0f, -1.0f); !pos) {
        ws::core::log_error(pos.error());
        source_ = {};
    }
}

void AudioDemo::draw_panel(ws::ui::gui::GuiContext& ctx,
                           ws::ui::gui::LayoutCursor& layout,
                           const ws::ui::gui::ChromeTheme& theme,
                           const ws::ui::gui::Rect& bounds) {
    namespace gui = ws::ui::gui;
    const auto page = gui::begin_panel(ctx, layout, theme, "sound", bounds);
    if (gui::button(ctx, layout, theme, "play", page.width)) {
        play();
    }
    gui::slider_float(ctx, layout, theme, "gain", gain_, 0.0f, 1.0f, page.width);
    gui::slider_float(ctx, layout, theme, "pan", pan_, -4.0f, 4.0f, page.width);
    gui::end_panel(ctx, layout);
}

bool AudioDemo::is_playing() const { return engine_.is_playing(source_); }

}  // namespace playground