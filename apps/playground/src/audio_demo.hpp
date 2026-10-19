// The sound panel demo: a synthesized arpeggio, monophonic playback, live
// gain/pan steering. Encapsulates the ws::audio handle lifecycle so
// main.cpp never touches engine/source validity directly.
#pragma once

#include <ws/audio/engine.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

namespace playground {

class AudioDemo {
public:
    // Init engine + synth/load the arpeggio. Failures are logged, not fatal -
    // the panel still draws, play() is a warned no-op.
    void setup();

    // Sound panel: play button + gain/pan sliders (sliders write through to
    // gain_/pan_, consumed by tick()).
    void draw_panel(ws::ui::gui::GuiContext&,
                    ws::ui::gui::LayoutCursor&,
                    const ws::ui::gui::ChromeTheme&,
                    const ws::ui::gui::Rect& bounds);

    // Per-frame: reap finished source, push current slider state to the
    // live source. Errors clear the source handle rather than logging every
    // frame.
    void tick();

    bool is_playing() const;

private:
    void play();

    ws::audio::AudioEngine engine_;
    ws::audio::SoundHandle arpeggio_{};
    ws::audio::SourceHandle source_{};
    bool ready_ = false;
    bool warned_ = false;
    float gain_ = 0.5f;
    float pan_ = 0.0f;
};

}  // namespace playground