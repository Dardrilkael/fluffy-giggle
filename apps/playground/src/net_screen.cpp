// The net screen's two halves (see net_screen.hpp): the world-space graph
// and the gui panel. Kept out of main.cpp so the screen reads as one
// self-contained integration piece - the gui library's first real screen.

#include "net_screen.hpp"

#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

#include <algorithm>
#include <cmath>

namespace playground {
namespace {

using ws::ui::gui::Rect;
using ws::ui::r2d::rgba;

// Node geometry, world space: one column per layer, squares spaced down
// the column. Chosen to sit in the free band of the default camera view,
// clear of the demo collage's big quads (the thin demo lines may cross -
// the net draws on top of them, it is the newest tenant).
constexpr float kNode = 28.0f;
constexpr float kColX[] = {520.0f, 640.0f, 740.0f};
constexpr float kTopY[] = {560.0f, 520.0f, 648.0f};
constexpr float kStrideY[] = {44.0f, 38.0f, 64.0f};

Rect node_rect(int layer, int index) {
    return {kColX[layer],
            kTopY[layer] + kStrideY[layer] * static_cast<float>(index),
            kNode, kNode};
}

// A -1..1 activation as a face fill: the idle control slate at 0, the
// theme's green accent at full positive swing, red at full negative. The
// fill IS the readout - the gui still has no text, so the graph carries
// the numbers as color.
std::uint32_t activation_face(float v) {
    const float t = std::min(std::abs(v), 1.0f);
    const bool positive = v >= 0.0f;
    const int lo[] = {70, 73, 78};  // control_bg - idle looks like a widget
    const int hi[] = {positive ? 90 : 200, positive ? 168 : 90,
                      positive ? 110 : 90};
    std::uint8_t ch[3];
    for (int i = 0; i < 3; ++i) {
        ch[i] = static_cast<std::uint8_t>(lo[i] +
                                          static_cast<int>((hi[i] - lo[i]) *
                                                           t));
    }
    return rgba(ch[0], ch[1], ch[2]);
}

}  // namespace

NetScreen::NetScreen() {
    hidden_on_.fill(true);
    // Non-zero defaults so the screen is lit on the first frame.
    in_ = {0.9f, 0.25f, 0.6f, 0.05f};
    reseed();
}

void NetScreen::reseed() {
    for (auto& row : w0_) {
        for (float& w : row) {
            w = next_weight();
        }
    }
    for (auto& row : w1_) {
        for (float& w : row) {
            w = next_weight();
        }
    }
}

float NetScreen::next_weight() {
    rng_ = rng_ * 1664525u + 1013904223u;  // wraps, by design (unsigned)
    return static_cast<float>(rng_ >> 8) / 16777216.0f * 2.0f - 1.0f;
}

void NetScreen::draw_world() {
    namespace r2d = ws::ui::r2d;

    // The forward pass, every frame (cheap; immediate mode re-reads state
    // per frame, so there is nothing to invalidate). A hidden node turned
    // off in the inspector contributes nothing - drawn dim, as if dead.
    for (int h = 0; h < kHidden; ++h) {
        float sum = 0.0f;
        for (int i = 0; i < kInputs; ++i) {
            sum += in_[static_cast<std::size_t>(i)] * w0_[h][i];
        }
        hid_[static_cast<std::size_t>(h)] =
            hidden_on_[static_cast<std::size_t>(h)] ? std::tanh(sum) : 0.0f;
    }
    for (int o = 0; o < kOutputs; ++o) {
        float sum = 0.0f;
        for (int h = 0; h < kHidden; ++h) {
            sum += hid_[static_cast<std::size_t>(h)] * w1_[o][h];
        }
        out_[static_cast<std::size_t>(o)] = std::tanh(sum);
    }

    // Edges first, so the nodes sit on their endpoints. Color carries the
    // sign (blue positive, red negative), alpha the magnitude; a dead
    // hidden node's edges fade with it.
    for (int h = 0; h < kHidden; ++h) {
        const Rect hidden = node_rect(1, h);
        const float gate = hidden_on_[static_cast<std::size_t>(h)] ? 1.0f
                                                                  : 0.25f;
        for (int i = 0; i < kInputs; ++i) {
            const float w = w0_[h][i];
            const auto alpha = static_cast<std::uint8_t>(
                (40.0f + 180.0f * std::min(std::abs(w), 1.0f)) * gate);
            const std::uint32_t color = w >= 0.0f
                                            ? rgba(120, 200, 255, alpha)
                                            : rgba(255, 120, 130, alpha);
            const Rect input = node_rect(0, i);
            r2d::draw_line(input.x + input.width,
                           input.y + input.height * 0.5f, hidden.x,
                           hidden.y + hidden.height * 0.5f, 2.0f, color);
        }
        for (int o = 0; o < kOutputs; ++o) {
            const float w = w1_[o][h];
            const auto alpha = static_cast<std::uint8_t>(
                (40.0f + 180.0f * std::min(std::abs(w), 1.0f)) * gate);
            const std::uint32_t color = w >= 0.0f
                                            ? rgba(120, 200, 255, alpha)
                                            : rgba(255, 120, 130, alpha);
            const Rect output = node_rect(2, o);
            r2d::draw_line(hidden.x + hidden.width,
                           hidden.y + hidden.height * 0.5f, output.x,
                           output.y + output.height * 0.5f, 2.0f, color);
        }
    }

    // The nodes, activation-colored; a dead hidden node keeps the idle
    // slate instead of the zero-activation face, so "off" reads as
    // disabled, not as merely quiet.
    for (int i = 0; i < kInputs; ++i) {
        const Rect r = node_rect(0, i);
        r2d::draw_rounded_quad(
            r.x, r.y, r.width, r.height, 6.0f,
            activation_face(in_[static_cast<std::size_t>(i)]));
    }
    for (int h = 0; h < kHidden; ++h) {
        const Rect r = node_rect(1, h);
        const std::uint32_t face =
            hidden_on_[static_cast<std::size_t>(h)]
                ? activation_face(hid_[static_cast<std::size_t>(h)])
                : rgba(48, 50, 55);
        r2d::draw_rounded_quad(r.x, r.y, r.width, r.height, 6.0f, face);
    }
    for (int o = 0; o < kOutputs; ++o) {
        const Rect r = node_rect(2, o);
        r2d::draw_rounded_quad(
            r.x, r.y, r.width, r.height, 6.0f,
            activation_face(out_[static_cast<std::size_t>(o)]));
    }
}

void NetScreen::draw_panel(ws::ui::gui::GuiContext& ctx,
                           ws::ui::gui::LayoutCursor& layout,
                           const ws::ui::gui::ChromeTheme& theme,
                           const ws::ui::r2d::FrameSize& frame) {
    namespace gui = ws::ui::gui;

    // Bottom-left, clear of the right-side demo panels.
    const Rect bounds{24.0f, frame.height - 232.0f, 240.0f, 208.0f};
    const Rect page = gui::begin_panel(ctx, layout, theme, "net", bounds);

    // The strip selects; the branch below is the per-tab content - the
    // composition the tab strip deliberately does not own.
    static const char* const kTabs[] = {"drive", "inspect"};
    (void)gui::tab_bar(ctx, layout, theme, "views", kTabs, 2, tab_,
                       page.width * 0.5f);
    const float below = theme.control_height + theme.spacing;
    if (tab_ == 0) {
        // Drive: one slider per input, id by index (same-label rows).
        for (int i = 0; i < kInputs; ++i) {
            ctx.ids().push(i);
            gui::slider_float(ctx, layout, theme, "in",
                              in_[static_cast<std::size_t>(i)], 0.0f, 1.0f,
                              page.width);
            ctx.ids().pop();
        }
        if (gui::button(ctx, layout, theme, "reseed", page.width)) {
            reseed();
        }
    } else {
        // Inspect: every hidden node's enable checkbox inside a scroll
        // region - eight rows against a shorter viewport, so the wheel
        // has real content to move.
        (void)gui::begin_scroll_panel(
            ctx, layout, theme, "hidden",
            {page.x, page.y + below, page.width, page.height - below});
        for (int h = 0; h < kHidden; ++h) {
            ctx.ids().push(h);
            (void)gui::checkbox(ctx, layout, theme, "node",
                                hidden_on_[static_cast<std::size_t>(h)]);
            ctx.ids().pop();
        }
        gui::end_scroll_panel(ctx, layout, theme);
    }
    gui::end_panel(ctx, layout);
}

}  // namespace playground
