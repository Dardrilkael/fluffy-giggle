// Phase 10 integration screen: a live "net lab". The net actually learns a
// 2D classification task (circle / xor / wave) with SGD + backprop, every
// frame, while the screen shows what is happening: the graph with signal
// pulses along the edges, the decision-boundary heatmap with the data on
// top, a loss sparkline and an accuracy bar. The panel steers it: learning
// rate, speed, dataset, reseed, and a model tab that ablates input features
// and hidden nodes so you can watch the net break (or not).
//
// The screen's seams are the old ones (draw_panel / draw_world /
// panel_bounds / set_ui_scale); the one addition is tick(), which
// main.cpp calls once per frame before draw_world(). All widget state is
// caller-owned (the gui contract); the net is plain app state.

#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/renderer2d.hpp>

#include <array>
#include <chrono>
#include <cstdint>

namespace playground {

class NetScreen {
public:
    NetScreen();

    // Advance simulation by the frame's dt (internally clocked and clamped
    // to 50 ms so a breakpoint doesn't become a training burst). Call once
    // per frame, before draw_world().
    void tick();

    // Render the world half: card, graph + pulses, heatmap, data, sparkline,
    // accuracy bar. Pure read of the state tick() left behind. Call inside
    // the camera'd r2d block, after draw_panel in the same frame.
    void draw_world();

    // Bottom-left gui panel. Tab "train" / "model" as before.
    void draw_panel(ws::ui::gui::GuiContext& ctx,
                    ws::ui::gui::LayoutCursor& layout,
                    const ws::ui::gui::ChromeTheme& theme,
                    const ws::ui::r2d::FrameSize& frame);

    const ws::ui::gui::Rect& panel_bounds() const { return panel_bounds_; }
    void set_ui_scale(float scale) { ui_scale_ = scale; }

private:
    // ---- training internals ----
    static constexpr int kInputs = 4;
    static constexpr int kHidden = 8;
    static constexpr int kSamples = 160;
    static constexpr int kHist = 96;
    static constexpr int kGrid = 20;

    void reseed();
    void make_dataset();
    float next_rand();
    void features(float x, float y, float f[kInputs]) const;
    float forward(const float f[kInputs], float* hid) const;
    void train_step(int sample);
    void evaluate();
    float learning_rate() const;

    // ---- draw helpers (each reads current state, no mutation) ----
    void draw_card();
    void draw_graph();
    void draw_heatmap();
    void draw_telemetry();

    // ---- state ----
    std::uint32_t rng_ = 0x1234u;
    float w0_[kHidden][kInputs]{};
    float b0_[kHidden]{};
    float w1_[kHidden]{};
    float b1_ = 0.0f;
    std::array<bool, kInputs> input_on_{};
    std::array<bool, kHidden> hidden_on_{};
    float sx_[kSamples]{}, sy_[kSamples]{};
    float label_[kSamples]{};
    int dataset_ = 0;
    float feat_[kInputs]{};
    std::array<float, kHidden> hid_{};
    float out_ = 0.0f;
    int probe_ = 0;
    float loss_ = 1.0f, acc_ = 0.5f;
    float hist_[kHist]{};
    int hist_head_ = 0;
    int eval_countdown_ = 0;
    int cursor_ = 0;
    long steps_total_ = 0;
    float lr_slider_ = 0.5f;
    float speed_slider_ = 0.5f;
    int tab_ = 0;
    float time_ = 0.0f;
    float step_accum_ = 0.0f;
    std::chrono::steady_clock::time_point last_tp_{};
    bool have_tp_ = false;
    ws::ui::gui::Rect panel_bounds_{};
    float ui_scale_ = 1.0f;
};

}  // namespace playground
