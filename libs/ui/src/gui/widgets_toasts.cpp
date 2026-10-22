// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the toast stack - the notification stratum (Layer::toast, 50, above
// the modal veil): a host-owned store drawn bottom-right, aging on the
// frame clock, each toast a panel wearing its severity band, its text
// and - when it carries one - a real action button.

#include "widgets_detail.hpp"

#include <ws/core/log.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace ws::ui::gui {

namespace detail {
// gui.h externs this; reset_warn_once() re-arms it (the test seam).
bool toast_overflow_warned = false;
}  // namespace detail

// --- the store ------------------------------------------------------------
//
// Out-of-line bodies for the widgets.hpp store: fixed slots, no
// GuiContext anywhere in here - push from wherever the app learns the
// news; only draw_toasts touches the context.

int Toasts::push(const Toast& toast) {
    // The lowest free slot first.
    int slot = -1;
    for (int i = 0; i < kMaxToasts; ++i) {
        if (!slots_[static_cast<std::size_t>(i)].live) {
            slot = i;
            break;
        }
    }
    bool evicted = false;
    if (slot < 0) {
        // The flood policy: past the cap the OLDEST toast drops - the
        // greatest age, a tie to the lowest slot (the earlier push) -
        // and the push takes its place. The drop is the point, not a
        // failure; warned once per process.
        evicted = true;
        slot = 0;
        for (int i = 1; i < kMaxToasts; ++i) {
            if (slots_[static_cast<std::size_t>(i)].age >
                slots_[static_cast<std::size_t>(slot)].age) {
                slot = i;
            }
        }
        if (!detail::toast_overflow_warned) {
            detail::toast_overflow_warned = true;
            core::warn(
                "ui: gui toast stack full at {} live - a push drops the oldest",
                kMaxToasts);
        }
    }
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    s.toast = toast;
    s.age = 0.0f;
    s.live = true;
    if (!evicted) {
        ++live_count_;  // an eviction replaced a live slot: the count stands
    }
    return slot;
}

void Toasts::dismiss(int slot) {
    if (slot < 0 || slot >= kMaxToasts) {
        return;
    }
    Slot& s = slots_[static_cast<std::size_t>(slot)];
    if (!s.live) {
        return;  // a stale id: a no-op
    }
    s.live = false;
    --live_count_;
}

void Toasts::clear() {
    for (Slot& s : slots_) {
        s.live = false;
    }
    live_count_ = 0;
}

int Toasts::count() const {
    return live_count_;
}

// --- the draw ---------------------------------------------------------------

int draw_toasts(GuiContext& gui, const ChromeTheme& theme, Toasts& toasts,
                float bottom_clearance) {
    // The aging pass, before anything draws: every live toast ages on
    // the frame clock, and one past its duration goes now - it never
    // draws again. A zero-or-less duration is sticky: only a click or
    // an explicit dismiss removes it.
    const ws::input::InputFrame& in = gui.input();
    for (int i = 0; i < Toasts::kMaxToasts; ++i) {
        Toasts::Slot& s = toasts.slots_[static_cast<std::size_t>(i)];
        if (!s.live) {
            continue;
        }
        s.age += in.dt_seconds;
        if (s.toast.duration > 0.0f && s.age >= s.toast.duration) {
            toasts.dismiss(i);
        }
    }
    if (toasts.count() <= 0) {
        return -1;
    }

    // The draw order: the YOUNGEST toast nearest the corner (age
    // ascending), a same-frame tie to the later push (the higher slot)
    // - so a fresh arrival lands at the corner and older ones ride up.
    std::array<int, Toasts::kMaxToasts> order{};
    int live = 0;
    for (int i = 0; i < Toasts::kMaxToasts; ++i) {
        if (toasts.slots_[static_cast<std::size_t>(i)].live) {
            order[static_cast<std::size_t>(live)] = i;
            ++live;
        }
    }
    std::sort(order.begin(), order.begin() + live, [&toasts](int a, int b) {
        const Toasts::Slot& sa = toasts.slots_[static_cast<std::size_t>(a)];
        const Toasts::Slot& sb = toasts.slots_[static_cast<std::size_t>(b)];
        if (sa.age != sb.age) {
            return sa.age < sb.age;
        }
        return a > b;
    });

    // The whole stack sits in ONE toast stratum, above the modal veil.
    gui.begin_layer(Layer::toast);
    const r2d::FrameSize frame = r2d::frame_size();
    // The toast stack takes no row in any caller's flow (the header's
    // contract: call it from wherever): a fresh local cursor carries
    // the per-toast panel scopes, and nothing of it survives the call.
    LayoutCursor layout;
    const float pad = theme.padding;
    const float toast_h = theme.control_height + 2.0f * pad;
    const float w_cap = std::max(0.0f, frame.width - 2.0f * pad);
    int fired = -1;  // the slot whose action fired, first in draw order
    float y = frame.height - bottom_clearance - toast_h;
    for (int oi = 0; oi < live; ++oi) {
        const int slot = order[static_cast<std::size_t>(oi)];
        Toasts::Slot& s = toasts.slots_[static_cast<std::size_t>(slot)];

        // --- the sizing -------------------------------------------------
        // The text and the action measured first, the panel sized to
        // them; a frame too narrow caps the panel and the text refits
        // into the room that is left.
        char text_scratch[128];
        LabelFit text_fit = fit_text(s.toast.text, -1.0f, theme, text_scratch,
                                     sizeof text_scratch);
        const bool has_action =
            s.toast.action != nullptr && s.toast.action[0] != '\0';
        const float gap = has_action ? theme.spacing : 0.0f;
        float button_w = 0.0f;
        if (has_action) {
            char action_scratch[128];
            const LabelFit action_fit =
                fit_text(s.toast.action, -1.0f, theme, action_scratch,
                         sizeof action_scratch);
            button_w = action_fit.advance + 2.0f * pad;
        }
        float toast_w = text_fit.advance +
                        (has_action ? gap + button_w : 0.0f) + 2.0f * pad;
        if (toast_w > w_cap) {
            toast_w = w_cap;  // the text refits into the cap's leavings
            const float room =
                std::max(0.0f, toast_w - 2.0f * pad -
                                     (has_action ? gap + button_w : 0.0f));
            text_fit = fit_text(s.toast.text, room, theme, text_scratch,
                                sizeof text_scratch);
        }
        const Rect bounds{frame.width - toast_w - pad, y, toast_w, toast_h};
        y -= toast_h + theme.spacing;  // the next toast rides up
        if (bounds.y < 0.0f) {
            continue;  // past the frame top: aged, never drawn
        }

        // --- the scope ----------------------------------------------------
        // Hand-rolled, not push_overlay_panel: a toast blocks nothing
        // (no wheel claim), and the severity band must paint the
        // panel's FULL height outside the content clip - so the escape
        // pair opens at the bounds, the chrome and band paint inside
        // it, and the intersecting content pair narrows the rows.
        r2d::push_clip_escape(bounds.x, bounds.y, bounds.width, bounds.height);
        gui.push_hit_clip_escape(bounds);
        panel_chrome(bounds, theme);
        std::uint32_t accent = theme.toast_info;
        if (s.toast.severity == ToastSeverity::warning) {
            accent = theme.toast_warning;
        } else if (s.toast.severity == ToastSeverity::error) {
            accent = theme.toast_error;
        }
        // The severity band: 3px down the left edge, the full panel
        // height - a hardcoded pixel like the selectable's accent bar.
        r2d::draw_quad(bounds.x, bounds.y, 3.0f, bounds.height, accent);
        const Rect content =
            layout.begin_panel(bounds, theme.padding, theme.spacing);
        r2d::push_clip(content.x, content.y, content.width, content.height);
        gui.push_hit_clip(content);
        gui.ids().push("toast");
        gui.ids().push(slot);

        // --- the row: the text, then the action on the same line --------
        // Laid from the content rect so the button's rect is known
        // before the call - the body-press test below excludes it.
        const float text_room =
            std::max(0.0f, content.width - (has_action ? gap + button_w : 0.0f));
        Rect button_rect{};
        bool action_fired = false;
        if (has_action) {
            text(layout, theme, s.toast.text, text_room);
            layout.same_line();  // one panel-spacing: the `gap` above
            button_rect = Rect{content.x + text_room + gap, content.y,
                               button_w, theme.control_height};
            // A real citizen at the toast stratum: focusable,
            // clickable, working above a modal veil (50 > 40 - the
            // layer numbers are the whole gate).
            action_fired =
                button(gui, layout, theme, s.toast.action, button_w);
        } else {
            text(layout, theme, s.toast.text, -1.0f);
        }

        // --- the dismissal -------------------------------------------------
        // The button's click, or a raw press edge on the body - the
        // bounds minus the button's rect. Raw, and deliberately NOT
        // claims_open()-gated: the toast stratum is above the modal
        // veil, so a toast dismisses while a dialog blocks the world
        // beneath. The press passes through - chrome is not an input
        // surface (the popup rim precedent): whatever sits beneath
        // still sees the same edge.
        bool body_pressed = false;
        if (in.mouse_present && in.mouse_pressed[0] &&
            bounds.contains(in.mouse_x, in.mouse_y) &&
            !(has_action && button_rect.contains(in.mouse_x, in.mouse_y))) {
            body_pressed = true;
        }
        const int touch_count =
            std::min(in.touch_count, ws::input::InputFrame::kMaxTouches);
        for (int t = 0; t < touch_count && !body_pressed; ++t) {
            const ws::input::TouchPoint& point =
                in.touches[static_cast<std::size_t>(t)];
            if (point.pressed && bounds.contains(point.x, point.y) &&
                !(has_action && button_rect.contains(point.x, point.y))) {
                body_pressed = true;
            }
        }
        if (action_fired) {
            toasts.dismiss(slot);
            if (fired < 0) {
                fired = slot;  // first in draw order wins
            }
        } else if (body_pressed) {
            // A body click is a dismissal, not an action: the toast
            // goes, the return stays quiet - an app's action reply
            // must never fire on a mere dismissal.
            toasts.dismiss(slot);
        }

        // --- the pops: the reverse of the pushes --------------------------
        gui.ids().pop();
        gui.ids().pop();
        gui.pop_hit_clip();  // the content pair
        r2d::pop_clip();
        layout.end_panel();
        gui.pop_hit_clip();  // the escape pair, opened at the bounds
        r2d::pop_clip();
    }
    gui.end_layer();
    return fired;
}

}  // namespace ws::ui::gui
