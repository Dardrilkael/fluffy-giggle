// Test-only: the r2d free-function seam, stubbed. The acceptance test
// links the gui archive (ws_ui_gui) with NO renderer behind it, so this
// TU provides every r2d:: symbol the gui references, recording each call
// into gui_test::stub so the tests can assert the gui really drew (and
// where its clips and layers went). draw_text keeps the real contract -
// it returns exactly the font's measure, so label paths behave - and
// frame_size returns the documented headless zero (the same contract
// renderer2d itself keeps outside a frame). If the gui ever grows a new
// r2d dependency, the link fails HERE, naming it: the acceptance fails
// loudly, not silently.

#include "ui_gui_stub.hpp"

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::gui_test {
StubDraws stub;
}  // namespace ws::ui::gui_test

namespace ws::ui::r2d {

FrameSize frame_size() {
    return {0.0f, 0.0f};  // the headless contract: empty, callers cope
}

void set_layer(std::uint32_t layer) {
    gui_test::stub.layer_sets++;
    gui_test::stub.last_layer = layer;
    if (layer > gui_test::stub.max_layer) {
        gui_test::stub.max_layer = layer;
    }
}

void push_clip(float, float, float, float) {
    gui_test::stub.clip_pushes++;
}

void push_clip_escape(float, float, float, float) {
    gui_test::stub.clip_pushes++;
}

void pop_clip() {
    gui_test::stub.clip_pops++;
}

void draw_quad(float, float, float, float, std::uint32_t color, Texture,
               float) {
    gui_test::stub.quads++;
    gui_test::stub.last_quad_color = color;
}

void draw_quad(float, float, float, float, float, std::uint32_t color,
               Texture) {
    gui_test::stub.quads++;  // the 3D billboard form, for completeness
    gui_test::stub.last_quad_color = color;
}

void draw_sprite(float, float, float, float, std::uint32_t, Sprite, float) {
    gui_test::stub.sprites++;
}

void draw_sprite(float, float, float, float, float, std::uint32_t, Sprite) {
    gui_test::stub.sprites++;  // the 3D billboard form, for completeness
}

void draw_rounded_quad(float, float, float, float, float radius,
                       std::uint32_t color, float) {
    gui_test::stub.rounded++;
    if (gui_test::stub.first_rounded_radius < 0.0f) {
        gui_test::stub.first_rounded_radius = radius;
    }
    gui_test::stub.last_rounded_color = color;
    gui_test::stub.last_rounded_radius = radius;
}

// The P5 primitives, mirrored at their own seams: the outline counts
// itself AND its two quads (the real renderer's draw_rounded_quad pair),
// the shadow itself and its four layers - so `rounded` stays the count
// of rounded quads the GPU would shade, whatever seam recorded them.
void draw_rounded_outline(float, float, float w, float h, float,
                          float thickness, std::uint32_t, std::uint32_t) {
    // The real gates, mirrored: no stroke or an empty rect is nothing,
    // and a stroke that eats the rect leaves the outer quad alone - the
    // headless count stays the count the GPU would shade.
    if (thickness <= 0.0f || w <= 0.0f || h <= 0.0f) {
        return;
    }
    gui_test::stub.outlines++;
    gui_test::stub.rounded += 1;
    if (w - 2.0f * thickness > 0.0f && h - 2.0f * thickness > 0.0f) {
        gui_test::stub.rounded += 1;
    }
}

void draw_shadow(float, float, float, float, float, float blur,
                 std::uint32_t color) {
    if (blur <= 0.0f || (color & 0xFFu) == 0u) {
        return;
    }
    gui_test::stub.shadows++;
    gui_test::stub.rounded += 4;
}

// The tint bracket: the stub records the push/pop pairing and the
// most recent tint so the motion suite can assert the overlay fades;
// it does not multiply (the stub records counts and a few lasts, not
// every color - the real multiply is the renderer's, tested by its
// own suite).
void push_tint(std::uint32_t tint) {
    gui_test::stub.tint_pushes++;
    gui_test::stub.last_tint = tint;
}

void pop_tint() {
    gui_test::stub.tint_pops++;
}

// The canvas layer's tessellation sink lands here (Renderer2DCanvas's
// R2DSink forwards every quad through this seam). The stub records the
// call so canvas-via-r2d tests can assert the quad count; the test
// suite mostly uses CommandListCanvas (no r2d), but the symbol must
// resolve so the pure archive links.
void draw_vertex_quad(const float (&)[4][2], const float (&)[4][2],
                      const float (&)[4][4], Texture) {
    gui_test::stub.quads++;
}

float draw_text(float, float, const char* utf8_text, const Font& font,
                std::uint32_t) {
    gui_test::stub.texts++;
    const float advance = font.measure_text(utf8_text);
    gui_test::stub.last_text_advance = advance;
    return advance;
}

}  // namespace ws::ui::r2d
