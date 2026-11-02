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
    std::uint32_t tint_pushes = 0;   // push_tint (the P4 overlay fades)
    std::uint32_t tint_pops = 0;     // pop_tint
    std::uint32_t last_tint = 0xFFFFFFFFu;  // the most recent push_tint color
    float last_text_advance = -1.0f;  // the most recent draw_text return
    // The P5 primitives, counted at their own seams: an outline records
    // itself here AND its two quads in `rounded` (the real renderer emits
    // exactly that pair of draw_rounded_quad calls; the shadow's four
    // layers land in `rounded` too), so one counter stays the whole truth.
    std::uint32_t outlines = 0;       // draw_rounded_outline calls
    std::uint32_t shadows = 0;        // draw_shadow calls
    // The most recent rounded-quad color (the P6 variant tests resolve
    // button faces through it - the stub keeps the last, the tests pin
    // the draw order they know).
    std::uint32_t last_rounded_color = 0;
    std::uint32_t last_quad_color = 0;
    // The most recent rounded-quad radius (the P5 pill tests resolve the
    // toggle track / scrollbar thumb radii through it).
    float last_rounded_radius = -1.0f;
    // The FIRST rounded-quad radius since the last reset - the toggle
    // draw's track (its knob paints after it), the scrollbar's thumb.
    float first_rounded_radius = -1.0f;

    void reset() {
        *this = StubDraws{};
        first_rounded_radius = -1.0f;
    }
};

extern StubDraws stub;

}  // namespace ws::ui::gui_test
