// Phase 10 integration screen: a "net inspector" - the neuralnet-studio
// flavor, one real screen's worth of gui driving a live forward pass.
// The world half draws the net (nodes + weighted edges) through r2d; the
// panel half is a full gui composition (tabs, sliders, button, a
// scrollable checkbox list) editing the same state. All widget state is
// caller-owned - the gui contract; the net itself is plain app state.

#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/renderer2d.hpp>

#include <array>
#include <cstdint>

namespace playground {

class NetScreen {
public:
    NetScreen();

    // The forward pass, then the world-space graph (edges first, nodes on
    // top). Call inside the camera'd r2d block. The gui block runs earlier
    // in the same frame, so this pass already reflects this frame's edits.
    void draw_world();

    // The gui side panel, bottom-left: tab "drive" holds the input sliders
    // and a reseed button, tab "inspect" a scrollable per-hidden-node
    // enable list (the scroll region has real room to scroll). Call inside
    // the screen-space r2d block, between new_frame and end_frame. The
    // theme is the caller's (third param, the widget convention) - its
    // font is what the labels draw with.
    void draw_panel(ws::ui::gui::GuiContext& ctx,
                    ws::ui::gui::LayoutCursor& layout,
                    const ws::ui::gui::ChromeTheme& theme,
                    const ws::ui::r2d::FrameSize& frame);

private:
    void reseed();
    float next_weight();  // deterministic LCG step, uniform in [-1, 1)

    static constexpr int kInputs = 4;
    static constexpr int kHidden = 8;
    static constexpr int kOutputs = 2;

    std::uint32_t rng_ = 0x1234u;
    std::array<float, kInputs> in_{};        // slider-driven, 0..1
    std::array<bool, kHidden> hidden_on_{};  // the inspector's checkboxes
    float w0_[kHidden][kInputs]{};
    float w1_[kOutputs][kHidden]{};
    std::array<float, kHidden> hid_{};  // this frame's activations
    std::array<float, kOutputs> out_{};
    int tab_ = 0;  // the panel's tab strip selection (caller state)
};

}  // namespace playground
