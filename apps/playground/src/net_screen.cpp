// The net lab's two halves (see net_screen.hpp): the world-space picture
// and the gui panel. Kept out of main.cpp so the screen reads as one
// self-contained integration piece - the gui library's first real screen.

#include "net_screen.hpp"

#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace playground {
namespace {

using ws::ui::gui::Rect;
using ws::ui::r2d::rgba;

// World-space layout. Everything hangs off one card so the screen reads as
// one object over the demo collage (the net is the newest tenant: it draws
// last and its card hides what it overlaps).
constexpr float kCardX = 500.0f, kCardY = 488.0f, kCardW = 500.0f, kCardH = 346.0f;
constexpr float kNode = 28.0f;
constexpr float kColX[] = {520.0f, 640.0f, 740.0f};
constexpr float kTopY[] = {560.0f, 520.0f, 648.0f};
constexpr float kStrideY[] = {44.0f, 38.0f, 64.0f};
constexpr float kMapX = 800.0f, kMapY = 520.0f, kMapSize = 180.0f;
constexpr float kPlotX = 800.0f, kPlotY = 716.0f, kPlotW = 180.0f, kPlotH = 40.0f;

const char* const kDatasetNames[] = {"circle", "xor", "wave"};
const char* const kInLabels[] = {"in x", "in y", "in x*y", "in x2+y2"};
const char* const kHidLabels[] = {"hidden 0", "hidden 1", "hidden 2", "hidden 3",
                                  "hidden 4", "hidden 5", "hidden 6", "hidden 7"};

Rect node_rect(int layer, int index) {
    return {kColX[layer], kTopY[layer] + kStrideY[layer] * static_cast<float>(index),
            kNode, kNode};
}

std::uint8_t u8(float v) {
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
}

// -1..1 -> slate at 0, green (+) / red (-) at full swing. The fill is the
// readout.
std::uint32_t activation_face(float v) {
    const float t = std::min(std::abs(v), 1.0f);
    const bool pos = v >= 0.0f;
    const float lo[] = {70, 73, 78};
    const float hi[] = {pos ? 90.0f : 200.0f, pos ? 168.0f : 90.0f, pos ? 110.0f : 90.0f};
    return rgba(u8(lo[0] + (hi[0] - lo[0]) * t), u8(lo[1] + (hi[1] - lo[1]) * t),
                u8(lo[2] + (hi[2] - lo[2]) * t));
}

// class color: -1 orange ... +1 blue
std::uint32_t class_color(float v, std::uint8_t a) {
    const float t = std::clamp(v * 0.5f + 0.5f, 0.0f, 1.0f);
    return rgba(u8(235 + (70 - 235) * t), u8(140 + (150 - 140) * t),
                u8(60 + (230 - 60) * t), a);
}

std::uint32_t off_face() { return rgba(48, 50, 55); }

}  // namespace

NetScreen::NetScreen() {
    input_on_ = {true, true, false, false};  // raw x,y: hidden nodes must earn it
    hidden_on_.fill(true);
    reseed();
}

float NetScreen::next_rand() {
    rng_ = rng_ * 1664525u + 1013904223u;  // wraps, by design (unsigned)
    return static_cast<float>(rng_ >> 8) / 16777216.0f * 2.0f - 1.0f;
}

void NetScreen::make_dataset() {
    for (int i = 0; i < kSamples; ++i) {
        const float x = next_rand(), y = next_rand();
        sx_[i] = x;
        sy_[i] = y;
        bool pos = false;
        switch (dataset_) {
            case 0: pos = x * x + y * y < 0.45f; break;           // circle
            case 1: pos = x * y > 0.0f; break;                    // xor
            default: pos = y > 0.6f * std::sin(3.0f * x); break;  // wave
        }
        label_[i] = pos ? 1.0f : -1.0f;
    }
}

void NetScreen::reseed() {
    for (auto& row : w0_) for (float& w : row) w = next_rand() * 0.8f;
    for (float& b : b0_) b = 0.0f;
    for (float& w : w1_) w = next_rand() * 0.8f;
    b1_ = 0.0f;
    make_dataset();
    std::fill(std::begin(hist_), std::end(hist_), 0.0f);
    hist_head_ = 0;
    cursor_ = 0;
    steps_total_ = 0;
    step_accum_ = 0.0f;
    evaluate();
}

void NetScreen::features(float x, float y, float f[kInputs]) const {
    f[0] = input_on_[0] ? x : 0.0f;
    f[1] = input_on_[1] ? y : 0.0f;
    f[2] = input_on_[2] ? x * y : 0.0f;
    f[3] = input_on_[3] ? x * x + y * y : 0.0f;
}

float NetScreen::forward(const float f[kInputs], float* hid) const {
    float s = b1_;
    for (int h = 0; h < kHidden; ++h) {
        float a = 0.0f;
        if (hidden_on_[static_cast<std::size_t>(h)]) {
            float p = b0_[h];
            for (int i = 0; i < kInputs; ++i) p += w0_[h][i] * f[i];
            a = std::tanh(p);
        }
        if (hid) hid[h] = a;
        s += w1_[h] * a;
    }
    return std::tanh(s);
}

float NetScreen::learning_rate() const {
    return std::pow(10.0f, -2.0f + 2.0f * lr_slider_);
}

// One SGD step on one sample; loss = 0.5 * (out - y)^2, tanh everywhere.
void NetScreen::train_step(int n) {
    float f[kInputs], hid[kHidden];
    features(sx_[n], sy_[n], f);
    const float o = forward(f, hid);
    const float lr = learning_rate();
    const float d_o = (o - label_[n]) * (1.0f - o * o);
    for (int h = 0; h < kHidden; ++h) {
        if (!hidden_on_[static_cast<std::size_t>(h)]) continue;  // ablated: no gradient
        const float d_h = d_o * w1_[h] * (1.0f - hid[h] * hid[h]);
        w1_[h] -= lr * d_o * hid[h];
        for (int i = 0; i < kInputs; ++i) w0_[h][i] -= lr * d_h * f[i];
        b0_[h] -= lr * d_h;
    }
    b1_ -= lr * d_o;
}

void NetScreen::evaluate() {
    float loss = 0.0f;
    int ok = 0;
    for (int n = 0; n < kSamples; ++n) {
        float f[kInputs];
        features(sx_[n], sy_[n], f);
        const float o = forward(f, nullptr);
        loss += 0.5f * (o - label_[n]) * (o - label_[n]);
        ok += (o > 0.0f) == (label_[n] > 0.0f);
    }
    loss_ = loss / static_cast<float>(kSamples);
    acc_ = static_cast<float>(ok) / static_cast<float>(kSamples);
    hist_[hist_head_] = loss_;
    hist_head_ = (hist_head_ + 1) % kHist;
}

// Simulation step: the body that used to open draw_world, minus the drawing.
void NetScreen::tick() {
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (have_tp_) {
        dt = std::min(std::chrono::duration<float>(now - last_tp_).count(), 0.05f);
    }
    last_tp_ = now;
    have_tp_ = true;
    time_ += dt;

    step_accum_ += dt * 100.0f * std::pow(60.0f, speed_slider_);
    const int steps = std::min(static_cast<int>(step_accum_), 400);
    step_accum_ -= static_cast<float>(static_cast<int>(step_accum_));
    for (int s = 0; s < steps; ++s) {
        train_step(cursor_);
        cursor_ = (cursor_ + 1) % kSamples;
    }
    steps_total_ += steps;
    if (--eval_countdown_ <= 0) {
        evaluate();
        eval_countdown_ = 4;
    }

    probe_ = static_cast<int>(time_ * 2.5f) % kSamples;
    features(sx_[probe_], sy_[probe_], feat_);
    out_ = forward(feat_, hid_.data());
}

void NetScreen::draw_world() {
    draw_card();
    draw_graph();
    draw_heatmap();
    draw_telemetry();
}

void NetScreen::draw_card() {
    namespace r2d = ws::ui::r2d;
    r2d::draw_rounded_quad(kCardX, kCardY, kCardW, kCardH, 10.0f,
                            rgba(20, 22, 26, 240));
    r2d::draw_rounded_quad(kCardX, kCardY, kCardW, 2.0f, 1.0f,
                            rgba(90, 168, 110));
}

void NetScreen::draw_graph() {
    namespace r2d = ws::ui::r2d;

    auto edge = [&](const Rect& a, const Rect& b, float w, float act,
                    float gate, float phase) {
        const float ax = a.x + a.width, ay = a.y + a.height * 0.5f;
        const float bx = b.x, by = b.y + b.height * 0.5f;
        const float mag = std::min(std::abs(w), 1.5f) / 1.5f;
        const auto alpha = u8((35.0f + 190.0f * mag) * gate);
        r2d::draw_line(ax, ay, bx, by, 1.0f + 2.5f * mag,
                       w >= 0.0f ? rgba(120, 200, 255, alpha)
                                 : rgba(255, 120, 130, alpha));
        const float flow = w * act;
        if (gate > 0.5f && std::abs(flow) > 0.06f) {
            const float t = std::fmod(time_ * 0.9f + phase, 1.0f);
            const float px = ax + (bx - ax) * t, py = ay + (by - ay) * t;
            const float r = 2.0f + 3.0f * std::min(std::abs(flow), 1.0f);
            r2d::draw_rounded_quad(px - r, py - r, 2 * r, 2 * r, r,
                                   flow >= 0.0f ? rgba(200, 235, 255, 230)
                                                : rgba(255, 200, 205, 230));
        }
    };
    for (int h = 0; h < kHidden; ++h) {
        const bool h_on = hidden_on_[static_cast<std::size_t>(h)];
        for (int i = 0; i < kInputs; ++i) {
            const bool both = h_on && input_on_[static_cast<std::size_t>(i)];
            edge(node_rect(0, i), node_rect(1, h), w0_[h][i], feat_[i],
                 both ? 1.0f : 0.2f, 0.13f * static_cast<float>(h * kInputs + i));
        }
        edge(node_rect(1, h), node_rect(2, 0), w1_[h],
             hid_[static_cast<std::size_t>(h)], h_on ? 1.0f : 0.2f,
             0.21f * static_cast<float>(h));
    }
    for (int i = 0; i < kInputs; ++i) {
        const Rect r = node_rect(0, i);
        r2d::draw_rounded_quad(r.x, r.y, r.width, r.height, 6.0f,
            input_on_[static_cast<std::size_t>(i)]
                ? activation_face(feat_[i]) : off_face());
    }
    for (int h = 0; h < kHidden; ++h) {
        const Rect r = node_rect(1, h);
        r2d::draw_rounded_quad(r.x, r.y, r.width, r.height, 6.0f,
            hidden_on_[static_cast<std::size_t>(h)]
                ? activation_face(hid_[static_cast<std::size_t>(h)]) : off_face());
    }
    {   // output: ring = truth for the probe, fill = what the net says
        const Rect r = node_rect(2, 0);
        r2d::draw_rounded_quad(r.x - 3, r.y - 3, r.width + 6, r.height + 6, 8.0f,
                                class_color(label_[probe_], 255));
        r2d::draw_rounded_quad(r.x, r.y, r.width, r.height, 6.0f,
                                class_color(out_, 255));
    }
}

void NetScreen::draw_heatmap() {
    namespace r2d = ws::ui::r2d;
    const float cell = kMapSize / static_cast<float>(kGrid);
    r2d::draw_rounded_quad(kMapX - 3, kMapY - 3, kMapSize + 6, kMapSize + 6,
                            4.0f, rgba(34, 36, 42));
    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const float x = (static_cast<float>(gx) + 0.5f) / kGrid * 2.0f - 1.0f;
            const float y = (static_cast<float>(gy) + 0.5f) / kGrid * 2.0f - 1.0f;
            float f[kInputs];
            features(x, y, f);
            r2d::draw_quad(kMapX + static_cast<float>(gx) * cell,
                            kMapY + static_cast<float>(gy) * cell,
                            cell + 0.5f, cell + 0.5f,
                            class_color(forward(f, nullptr), 190));
        }
    }
    for (int n = 0; n < kSamples; ++n) {
        const float px = kMapX + (sx_[n] * 0.5f + 0.5f) * kMapSize;
        const float py = kMapY + (sy_[n] * 0.5f + 0.5f) * kMapSize;
        r2d::draw_rounded_quad(px - 3.0f, py - 3.0f, 6, 6, 3.0f,
                                rgba(15, 15, 18, 255));
        r2d::draw_rounded_quad(px - 2.0f, py - 2.0f, 4, 4, 2.0f,
                                class_color(label_[n], 255));
    }
    {
        const float px = kMapX + (sx_[probe_] * 0.5f + 0.5f) * kMapSize;
        const float py = kMapY + (sy_[probe_] * 0.5f + 0.5f) * kMapSize;
        const float r = 7.0f + 1.5f * std::sin(time_ * 8.0f);
        r2d::draw_rounded_quad(px - r, py - r, 2 * r, 2 * r, r,
                                rgba(255, 255, 255, 110));
    }
}

void NetScreen::draw_telemetry() {
    namespace r2d = ws::ui::r2d;
    r2d::draw_rounded_quad(kPlotX, kPlotY, kPlotW, kPlotH, 4.0f,
                            rgba(34, 36, 42));
    float hmax = 1e-3f;
    for (float v : hist_) hmax = std::max(hmax, v);
    for (int k = 1; k < kHist; ++k) {
        const float a = hist_[(hist_head_ + k - 1) % kHist];
        const float b = hist_[(hist_head_ + k) % kHist];
        const float x0 = kPlotX + kPlotW * static_cast<float>(k - 1) / (kHist - 1);
        const float x1 = kPlotX + kPlotW * static_cast<float>(k) / (kHist - 1);
        r2d::draw_line(x0, kPlotY + kPlotH - 3 - (kPlotH - 6) * a / hmax,
                        x1, kPlotY + kPlotH - 3 - (kPlotH - 6) * b / hmax,
                        1.5f, rgba(255, 210, 90));
    }
    r2d::draw_rounded_quad(kPlotX, kPlotY + kPlotH + 6, kPlotW, 6, 3.0f,
                            rgba(34, 36, 42));
    r2d::draw_rounded_quad(kPlotX, kPlotY + kPlotH + 6,
                            std::max(kPlotW * acc_, 6.0f), 6, 3.0f,
                            rgba(90, 200, 120));
}

void NetScreen::draw_panel(ws::ui::gui::GuiContext& ctx,
                           ws::ui::gui::LayoutCursor& layout,
                           const ws::ui::gui::ChromeTheme& theme,
                           const ws::ui::r2d::FrameSize& frame) {
    namespace gui = ws::ui::gui;
    const ws::ui::r2d::Font* font = theme.font;  // null = fontless theme

    const Rect bounds{24.0f * ui_scale_, frame.height - 232.0f * ui_scale_,
                      240.0f * ui_scale_, 208.0f * ui_scale_};
    panel_bounds_ = bounds;

    // Status line just above the panel: the numbers the graph encodes as color.
    if (font != nullptr) {
        char line[96];
        std::snprintf(line, sizeof(line), "%s  loss %.3f  acc %d%%  %ldk steps",
                      kDatasetNames[dataset_], static_cast<double>(loss_),
                      static_cast<int>(acc_ * 100.0f + 0.5f), steps_total_ / 1000);
        ws::ui::r2d::draw_text(bounds.x, bounds.y - 22.0f * ui_scale_, line, *font,
                               rgba(200, 205, 212));
    }

    const Rect page = gui::begin_panel(ctx, layout, theme, "net", bounds);
    static const char* const kTabs[] = {"train", "model"};
    (void)gui::tab_bar(ctx, layout, theme, "views", kTabs, 2, tab_,
                       page.width * 0.5f);
    const float below = theme.control_height + theme.spacing;

    if (tab_ == 0) {
        gui::slider_float(ctx, layout, theme, "lr", lr_slider_, 0.0f, 1.0f, page.width);
        gui::slider_float(ctx, layout, theme, "speed", speed_slider_, 0.0f, 1.0f,
                          page.width);
        if (gui::button(ctx, layout, theme, "next dataset", page.width)) {
            dataset_ = (dataset_ + 1) % 3;
            reseed();
        }
        if (gui::button(ctx, layout, theme, "reseed", page.width)) reseed();
    } else {
        // 4 input features + 8 hidden nodes against a short viewport: the
        // wheel (or a finger) has real content to move.
        (void)gui::begin_scroll_panel(
            ctx, layout, theme, "model",
            {page.x, page.y + below, page.width, page.height - below});
        for (int i = 0; i < kInputs; ++i) {
            (void)gui::checkbox(ctx, layout, theme, kInLabels[i],
                                input_on_[static_cast<std::size_t>(i)]);
        }
        for (int h = 0; h < kHidden; ++h) {
            (void)gui::checkbox(ctx, layout, theme, kHidLabels[h],
                                hidden_on_[static_cast<std::size_t>(h)]);
        }
        gui::end_scroll_panel(ctx, layout, theme);
    }
    gui::end_panel(ctx, layout);
}

}  // namespace playground
