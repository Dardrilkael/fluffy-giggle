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

void popup_frame_draw(const Rect& bounds, const ChromeTheme& theme) {
    // The overlay window's chrome (P5): a themed shadow first - the 4
    // layer stack under the window, gated on the theme's own shadow
    // tokens (blur 0 = the flat stock of the pre-P5 look, and the
    // custom-skin spelling of "none") - then the border as a rounded
    // outline at the POPUP radius, the same panel_chrome pair at the
    // popup's own role radius. Drawn inside the caller's clip escape,
    // which push_overlay_panel (and the tooltip/toast scopes) opens on
    // shadow_bounds so the spread paints past the window rect without
    // ever painting past the host panel.
    if (theme.shadow_blur > 0.0f) {
        r2d::draw_shadow(bounds.x, bounds.y + theme.shadow_offset_y,
                         bounds.width, bounds.height, theme.popup_radius(),
                         theme.shadow_blur, theme.shadow);
    }
    r2d::draw_rounded_outline(bounds.x, bounds.y, bounds.width, bounds.height,
                              theme.popup_radius(),
                              std::max(1.0f, theme.border_width),
                              theme.border, theme.panel_bg);
}

bool begin_popup(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& anchor, const Rect& bounds, bool& open) {
    if (!open) {
        // A closed frame re-arms the fade: the NEXT open starts from
        // nothing again (the state cannot reset while closed - nothing
        // runs then - so the closed branch does it).
        gui.persistent_state<OverlayFade>(overlay_fade_id(gui.ids(), label))
            .was_open = false;
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
    // P4: the fade-in bracket - one tint over the whole window (frame,
    // children and all), a fresh open starting from nothing. INSTANT
    // (the identity tint) whenever the theme's motion is off. A fresh
    // open resets the traveler itself (the value under this id, not
    // just a flag): the fade replays from 0 every time the window
    // reopens. The id is the OVERLAY'S own (a reserved key inside the
    // label's scope - overlay_fade_id), so the clear is surgical: the
    // anchor widget's state under the label id - a combo row's face
    // traveler - survives the open untouched.
    const WidgetId pid = overlay_fade_id(gui.ids(), label);
    if (!gui.persistent_state<OverlayFade>(pid).was_open) {
        gui.reset_animation(pid);  // the traveler restarts, nothing else
    }
    gui.persistent_state<OverlayFade>(pid).was_open = true;
    const float t =
        gui.animate_toward(pid, 1.0f, motion_duration(theme));
    r2d::push_tint(fade_tint(t));
    push_overlay_panel(gui, layout, theme, label, bounds, theme.spacing);
    return open;
}

void end_popup(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer: a begin_popup that answered false
    // pushed nothing, and this guard keeps a stray end from eating an
    // outer scope. The fade bracket pops with the scope it wrapped.
    if (gui.current_layer() != Layer::popup) {
        return;
    }
    pop_overlay_panel(gui, layout);
    r2d::pop_tint();
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
        // The closed frame re-arms the fade, like a popup's - under the
        // tooltip's own key, the same id the open branch keys.
        gui.persistent_state<OverlayFade>(overlay_fade_id(gui.ids(), label))
            .was_open = false;
        return false;
    }

    // The tooltip stratum and the panel scope, with the clip escape on
    // the r2d side only: the hint draws past any panel it hovers over.
    // NO hit clip, no wheel claim - nothing in a tooltip is meant to
    // claim, and a caller who puts a live widget in one asked for exactly
    // what they get (it claims at the tooltip stratum, unclipped).
    gui.begin_layer(Layer::tooltip);
    // P4: the fade-in bracket, a popup's own spelling (the fresh-open
    // reset included) - but keyed the overlay's OWN id, a reserved key
    // inside the label's scope (overlay_fade_id). The anchor widget
    // shares this label: its rest clock (tooltip_delayed) and face
    // travelers live under the label id, and the fresh-open clear must
    // restart the fade without erasing them - the clock's blink-out
    // was the first-hover flicker.
    const WidgetId pid = overlay_fade_id(gui.ids(), label);
    if (!gui.persistent_state<OverlayFade>(pid).was_open) {
        gui.reset_animation(pid);  // the traveler restarts
    }
    gui.persistent_state<OverlayFade>(pid).was_open = true;
    const float t =
        gui.animate_toward(pid, 1.0f, motion_duration(theme));
    r2d::push_tint(fade_tint(t));
    // P5: the escape opens on the shadow's bounds - the same rule
    // push_overlay_panel spells - so a themed shadow spreads past the
    // hint rect without painting past the host panel.
    const Rect escape = shadow_bounds(bounds, theme);
    r2d::push_clip_escape(escape.x, escape.y, escape.width, escape.height);
    paint(theme, &WidgetPainters::popup_frame, &popup_frame_draw, bounds,
          theme);
    layout.begin_panel(bounds, theme.padding, theme.spacing);
    gui.ids().push(label);
    return show;
}

void end_tooltip(GuiContext& gui, LayoutCursor& layout) {
    // Drop-safe through the layer, like end_popup. The fade bracket
    // pops with the scope it wrapped.
    if (gui.current_layer() != Layer::tooltip) {
        return;
    }
    gui.ids().pop();
    layout.end_panel();
    r2d::pop_clip();
    r2d::pop_tint();
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
