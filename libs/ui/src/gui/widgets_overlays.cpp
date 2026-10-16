// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the overlay strata - popup, tooltip and modal, the scopes above the
// panel flow: the layer push, the clip escape, and for the modal the
// veil that owns every claim beneath it.

#include "widgets_detail.hpp"

#include <algorithm>

namespace ws::ui::gui {

bool begin_popup(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& anchor, const Rect& bounds, bool& open) {
    if (!open) {
        return false;
    }

    // The outside-press close: a fresh press that lands outside the bounds
    // AND outside the anchor flips `open` false and lays nothing this
    // frame - the press carries on to whatever sits beneath (the click
    // passes through; the widget under claims it fresh). The anchor is
    // exempt so the click that hangs the popup on its own control belongs
    // to that control (a combo row toggles itself), not to the close
    // rule. The veil gate applies: under a raised modal veil the popup is
    // inert like every lower stratum - its close rule eats no press the
    // veil owns.
    const ws::input::InputFrame& input = gui.input();
    if (gui.claims_open()) {
        if (input.mouse_present && input.mouse_pressed[0] &&
            !bounds.contains(input.mouse_x, input.mouse_y) &&
            !anchor.contains(input.mouse_x, input.mouse_y)) {
            open = false;
            return false;
        }
        const int touch_count =
            std::min(input.touch_count, ws::input::InputFrame::kMaxTouches);
        for (int i = 0; i < touch_count; ++i) {
            const ws::input::TouchPoint& point =
                input.touches[static_cast<std::size_t>(i)];
            if (point.pressed && !bounds.contains(point.x, point.y) &&
                !anchor.contains(point.x, point.y)) {
                open = false;
                return false;
            }
        }
    }

    // The stratum, then the panel scope. The clip pair ESCAPES at the
    // bounds, r2d side and hit side at the same rect like every panel
    // clip: the popup hangs off a widget inside a panel (whose clip pair
    // is active at this call), and it must draw AND hit-test past its
    // host's hole - a dropdown at the host's bottom edge would shear off
    // and go unclickable there otherwise. The content pair then nests
    // against the escaped bounds as usual.
    gui.begin_layer(Layer::popup);
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    gui.push_hit_clip_escape(bounds);
    panel_chrome(bounds, theme);
    const Rect content =
        layout.begin_panel(bounds, theme.padding, theme.spacing);
    // Keyless wheel claim, begin_panel's: claims to eat, never consume -
    // a popup over a scroll panel takes the wheel even though it scrolls
    // nothing.
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return open;
}

void end_popup(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer: a begin_popup that answered false
    // pushed nothing, and this guard keeps a stray end from eating an
    // outer scope.
    if (gui.current_layer() != Layer::popup) {
        return;
    }
    // Exactly the reverse of begin_popup's pushes.
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    r2d::pop_clip();
    gui.pop_hit_clip();
    gui.end_layer();
}

bool begin_tooltip(GuiContext& gui, LayoutCursor& layout,
                   const ChromeTheme& theme, const char* label,
                   const Rect& bounds, bool show) {
    if (!show) {
        return false;
    }

    // The tooltip stratum and the panel scope, with the clip escape on
    // the r2d side only: the hint draws past any panel it hovers over.
    // NO hit clip, no wheel claim - nothing in a tooltip is meant to
    // claim, and a caller who puts a live widget in one asked for exactly
    // what they get (it claims at the tooltip stratum, unclipped).
    gui.begin_layer(Layer::tooltip);
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    panel_chrome(bounds, theme);
    layout.begin_panel(bounds, theme.padding, theme.spacing);
    gui.ids().push(label);
    return show;
}

void end_tooltip(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup.
    if (gui.current_layer() != Layer::tooltip) {
        return;
    }
    gui.ids().pop();
    layout.end_panel();
    r2d::pop_clip();
    gui.end_layer();
}

bool begin_modal(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds, bool open) {
    if (!open) {
        return false;
    }

    gui.begin_layer(Layer::modal);

    // The visible veil - what the input rules already enforce, painted.
    // Drawn first inside the stratum (above every lower-layer chrome,
    // before this scope's own panel) and under its own FULLSCREEN clip
    // escape: the modal may open while a host panel's clip is active, and
    // the veil covers the whole frame, not the host's content rect.
    // Headless the frame size is zero - the escape normalizes to empty
    // and the veil drops, the ordinary r2d contract.
    const r2d::FrameSize frame = r2d::frame_size();
    r2d::push_clip_escape(0.0f, 0.0f, frame.width, frame.height);
    r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, theme.veil);
    r2d::pop_clip();

    // Then the panel scope, the same shape as a popup's - the clip pair
    // escapes at the bounds, the content pair nests inside - minus the
    // close rule: blocking IS the point. The dialog's own buttons flip
    // the caller's bool.
    r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
    gui.push_hit_clip_escape(bounds);
    panel_chrome(bounds, theme);
    const Rect content =
        layout.begin_panel(bounds, theme.padding, theme.spacing);
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return open;
}

void end_modal(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup. Note the veil the
    // scope raised outlives this end - frame-wide, lifts at the next
    // begin_frame (the begin_layer rules).
    if (gui.current_layer() != Layer::modal) {
        return;
    }
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
    r2d::pop_clip();
    gui.pop_hit_clip();
    gui.end_layer();
}

}  // namespace ws::ui::gui
