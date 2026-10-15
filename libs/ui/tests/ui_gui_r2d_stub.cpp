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

void draw_quad(float, float, float, float, std::uint32_t, Texture, float) {
    gui_test::stub.quads++;
}

void draw_quad(float, float, float, float, float, std::uint32_t, Texture) {
    gui_test::stub.quads++;  // the 3D billboard form, for completeness
}

void draw_sprite(float, float, float, float, std::uint32_t, Sprite, float) {
    gui_test::stub.sprites++;
}

void draw_sprite(float, float, float, float, float, std::uint32_t, Sprite) {
    gui_test::stub.sprites++;  // the 3D billboard form, for completeness
}

void draw_rounded_quad(float, float, float, float, float, std::uint32_t,
                       float) {
    gui_test::stub.rounded++;
}

float draw_text(float, float, const char* utf8_text, const Font& font,
                std::uint32_t) {
    gui_test::stub.texts++;
    const float advance = font.measure_text(utf8_text);
    gui_test::stub.last_text_advance = advance;
    return advance;
}

}  // namespace ws::ui::r2d
