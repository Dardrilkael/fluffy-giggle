#pragma once

// Test-only: the recording state behind the stub r2d seam in
// ui_gui_r2d_stub.cpp. The acceptance test asserts the gui really drew
// through the free-function seam - without that, "links without a
// renderer" could pass with the gui silently drawing nothing, so every
// stubbed call counts itself here and a few keep the values the tests
// reason about.

#include <cstdint>

namespace ws::ui::gui_test {

struct StubDraws {
    std::uint32_t quads = 0;          // draw_quad calls
    std::uint32_t rounded = 0;        // draw_rounded_quad calls
    std::uint32_t sprites = 0;        // draw_sprite calls
    std::uint32_t texts = 0;          // draw_text calls
    std::uint32_t clip_pushes = 0;    // push_clip + push_clip_escape
    std::uint32_t clip_pops = 0;      // pop_clip
    std::uint32_t layer_sets = 0;    // set_layer
    std::uint32_t last_layer = 0;     // the most recent set_layer value
    std::uint32_t max_layer = 0;      // the highest set_layer value
    float last_text_advance = -1.0f;  // the most recent draw_text return

    void reset() { *this = StubDraws{}; }
};

extern StubDraws stub;

}  // namespace ws::ui::gui_test
