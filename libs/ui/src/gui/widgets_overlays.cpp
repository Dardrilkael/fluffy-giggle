// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the overlay strata - popup, tooltip and modal, the scopes above the
// panel flow: the layer push, the clip escape, and for the modal the
// veil that owns every claim beneath it. This file also owns the
// placement arithmetic every overlay shares (place_popup) and the
// tooltip pair - the hover clock and the folded one-liner.

#include "widgets_detail.hpp"

#include <algorithm>

namespace ws::ui::gui {

Rect popup_placement(const Rect& anchor, float width, float height,
                     PopupAnchor side, const r2d::FrameSize& viewport) {
    // The viewport clamps the SIZE first: a window taller or wider than
    // the frame lays filling what fits, never backwards.
    width = std::max(0.0f, std::min(width, viewport.width));
    height = std::max(0.0f, std::min(height, viewport.height));

    float x = anchor.x;
    float y = anchor.y;
    if (side == PopupAnchor::below) {
        y = anchor.y + anchor.height;  // under the bottom edge...
        if (y + height > viewport.height) {
            y = anchor.y - height;  // ...unless the frame lacks the room
        }
    } else {
        x = anchor.x + anchor.width;  // past the right edge, or
        if (x + width > viewport.width) {
            x = anchor.x - width;  // flipped to the left when it cannot fit
        }
    }
    // Both axes clamp into the frame last. The flip already chose the
    // side; what remains is keeping the window on the frame - a
    // degenerate anchor (a point) clamps from wherever it sits.
    x = std::max(0.0f, std::min(x, viewport.width - width));
    y = std::max(0.0f, std::min(y, viewport.height - height));
    return {x, y, width, height};
}

Rect place_popup(const Rect& anchor, float width, float height,
                 PopupAnchor side) {
    return popup_placement(anchor, width, height, side, r2d::frame_size());
}

bool begin_popup(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& anchor, const Rect& bounds, bool& open) {
    if (!open) {
        return false;
    }

    // Escape from LAST frame's arbitration: end_frame resolved the
    // claims and published the winner, and this entry is where the
    // popup reads its win. The closing frame claims nothing - one
    // Escape closes one level, and whatever holds it open (a bar's
    // menu, a parent submenu) claims fresh the frame after, so the
    // next press climbs the chain a link at a time.
    if (gui.esc_won(&open)) {
        open = false;
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

    // The stratum, then this frame's Escape claim - AFTER the layer
    // push, so the arbitration records the popup's stratum, the same
    // value the hit rules and the draw order both speak - then the
    // shared overlay panel scope (the clip escapes, chrome, the wheel
    // claim, the content clips and the id push: the popup hangs off a
    // widget inside a panel whose clip pair is active here, and it
    // must draw AND hit-test past its host's hole).
    gui.begin_layer(Layer::popup);
    gui.claim_esc(&open);
    push_overlay_panel(gui, layout, theme, label, bounds, theme.spacing);
    return open;
}

void end_popup(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer: a begin_popup that answered false
    // pushed nothing, and this guard keeps a stray end from eating an
    // outer scope.
    if (gui.current_layer() != Layer::popup) {
        return;
    }
    pop_overlay_panel(gui, layout);
    gui.end_layer();
}

bool tooltip_delayed(GuiContext& gui, WidgetId id, const ChromeTheme& theme) {
    // The hover clock: rest accumulates while the widget is hot, and
    // the answer turns true the frame it passes the theme's delay.
    // is_hot is the SETTLED hover, so the clock starts on the settled
    // claim and a flickering hover - an overlapped row - never banks
    // enough rest to fire.
    struct RestState {
        float rest = 0.0f;  // seconds of settled hover, zeroed off-hover
    };
    RestState& state = gui.persistent_state<RestState>(id);
    if (!gui.is_hot(id)) {
        state.rest = 0.0f;
        return false;
    }
    state.rest += gui.input().dt_seconds;
    return state.rest >= theme.tooltip_delay;
}

void tooltip_text(GuiContext& gui, LayoutCursor& layout,
                  const ChromeTheme& theme, const char* label,
                  const Rect& anchor, const char* hint) {
    // The whole hover-to-hint pattern in one call: the delay (keyed by
    // the shared label - the explained widget is laid in this same
    // scope, so its id keys the clock), then a one-row tooltip placed
    // below the anchor's row, flipped and clamped into the frame,
    // sized to the hint plus the panel padding. The parameter spells
    // itself `hint` because `text` here is the row widget the scope's
    // one line lands in.
    const WidgetId id = gui.ids().id(label);
    if (!tooltip_delayed(gui, id, theme)) {
        return;
    }
    char scratch[128];
    const LabelFit fit =
        fit_text(hint, -1.0f, theme, scratch, sizeof scratch);
    if (fit.advance <= 0.0f) {
        return;  // nothing to say - a zero-size box is noise
    }
    const Rect bounds =
        place_popup(anchor, fit.advance + 2.0f * theme.padding,
                    theme.control_height + 2.0f * theme.padding);
    if (begin_tooltip(gui, layout, theme, label, bounds, true)) {
        text(layout, theme, hint, -1.0f);
        end_tooltip(gui, layout);
    }
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

    // Then the shared overlay panel scope, the same shape as a popup's,
    // minus the close rules: blocking IS the point. The dialog's own
    // buttons flip the caller's bool.
    push_overlay_panel(gui, layout, theme, label, bounds, theme.spacing);
    return open;
}

void end_modal(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup. Note the veil the
    // scope raised outlives this end - frame-wide, lifts at the next
    // begin_frame (the begin_layer rules).
    if (gui.current_layer() != Layer::modal) {
        return;
    }
    pop_overlay_panel(gui, layout);
    gui.end_layer();
}

}  // namespace ws::ui::gui
