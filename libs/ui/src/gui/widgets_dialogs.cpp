// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the modal dialog - the one-call overlay that owns its window end to
// end: the veil and panel through begin_modal, the title and body rows,
// the standard button set, and the Enter/Escape semantics, so a dialog
// is one call in the host's frame, not a scope it builds.

#include "widgets_detail.hpp"

#include <algorithm>
#include <functional>

namespace ws::ui::gui {
namespace {

// The standard button sets, one per DialogButtons value in enum order:
// the stock labels in reading order (the positive first) and how many
// rows the set lays. `none` carries no labels at all - the set that
// takes no keys and answers nothing.
struct StockButtons {
    const char* labels[2] = {"", ""};
    int count = 0;
};
constexpr StockButtons kStockButtons[] = {
    {{"", ""}, 0},              // none
    {{"OK", ""}, 1},            // ok
    {{"OK", "Cancel"}, 2},      // ok_cancel
    {{"Yes", "No"}, 2},         // yes_no
    {{"Retry", "Cancel"}, 2},   // retry_cancel
};
constexpr int kStockButtonsCount =
    static_cast<int>(sizeof(kStockButtons) / sizeof(kStockButtons[0]));

// modal_dialog's persistent half: the content extent measured at the
// last frame the dialog RAN (the one-frame settle a scroll panel's
// content extent uses), the once-ever `measured` flag separating the
// settle frame's estimate from every later frame's answer, and the
// open-carry the Enter rule reads. The extent persists across closes -
// "the LAST frame it ran" - so a reopened dialog opens at the size it
// left.
struct DialogState {
    float extent = 0.0f;
    bool measured = false;
    bool was_open = false;
};

}  // namespace

DialogEvent modal_dialog(GuiContext& gui, const ChromeTheme& theme,
                         const char* label, const char* title,
                         DialogButtons buttons, bool& open, float width,
                         const std::function<void(GuiContext&, LayoutCursor&,
                                                  const ChromeTheme&)>& body) {
    DialogEvent event;  // {-1, false}: nothing fired this frame
    const WidgetId id = gui.ids().id(label);
    DialogState& state = gui.persistent_state<DialogState>(id);
    if (!open) {
        state.was_open = false;  // the closed frames re-arm the Enter guard
        return event;
    }
    const bool freshly_opened = !state.was_open;
    state.was_open = true;

    // A cast out of the enum's range reads as `none` - the set that
    // answers nothing.
    const int set_index = static_cast<int>(buttons);
    const StockButtons& stock =
        set_index >= 0 && set_index < kStockButtonsCount
            ? kStockButtons[static_cast<std::size_t>(set_index)]
            : kStockButtons[0];
    const int button_count = stock.count;

    // Escape from LAST frame's arbitration: end_frame resolved the
    // claims and published the winner, and this entry is where the
    // dialog reads its win. The closing frame claims nothing - the next
    // Escape settles on whatever overlays are left. Escape answers the
    // negative: the negative button's own event when the set carries
    // one, else a plain dismissal - accepted false, no button firing.
    if (gui.esc_won(&open)) {
        open = false;
        state.was_open = false;
        event.button = button_count > 1 ? 1 : -1;
        event.accepted = false;
        return event;
    }

    // Enter answers the positive from wherever the focus sits - read
    // straight off the edge and spent HERE, before the scope opens, so
    // nothing in the body double-takes the key (Space stays a body
    // widget's own; a set that answers nothing takes neither key). The
    // frame that OPENED the dialog never answers it: the host's
    // activating key edge is still in that frame, and the Enter that
    // opened a confirm dialog must not confirm it - the was_open
    // carry, reset on every close, is the guard.
    if (button_count > 0 && !freshly_opened &&
        gui.input().key_pressed[ws::input::keys::kEnter]) {
        gui.consume_key(ws::input::keys::kEnter);
        open = false;
        state.was_open = false;
        event.button = 0;
        event.accepted = true;
        return event;
    }

    // The window: centered on the frame at `width` (a negative width
    // fills the frame, the set's one width convention), its height the
    // content extent the last run measured - the settle frame (nothing
    // measured yet) opens at the title + button row estimate and grows
    // into the body one frame later. Clamped to the frame; headless
    // the frame is empty and everything clamps to the origin.
    const r2d::FrameSize frame = r2d::frame_size();
    if (width < 0.0f) {
        width = frame.width;
    }
    width = std::max(0.0f, std::min(width, frame.width));
    const float settle = 2.0f * theme.control_height + theme.spacing;
    const float content_h = state.measured ? state.extent : settle;
    const float height = std::max(
        0.0f, std::min(content_h + 2.0f * theme.padding, frame.height));
    const Rect bounds{(frame.width - width) * 0.5f,
                      (frame.height - height) * 0.5f, width, height};

    // The dialog's OWN cursor: the window takes no row in any host
    // flow, so it opens its panel on a fresh stack and the body's rows
    // have a cursor to land in even though the host handed none in.
    LayoutCursor layout;
    if (!begin_modal(gui, layout, theme, label, bounds, open)) {
        return event;
    }

    // This frame's Escape claim, AFTER the layer push - the arbitration
    // records the modal stratum. A body text field that drops its
    // selection first spends the key: one Escape abandons the edit, the
    // next closes the dialog.
    if (button_count > 0) {
        gui.claim_esc(&open);
    }

    // The title: chrome, a plain heading row.
    text(layout, theme, title);

    // The body: any widget calls, in the dialog's own cursor and id
    // scope; the veil traps Tab inside whatever registers.
    if (body) {
        body(gui, layout, theme);
    }

    // The button row: one uniform width - the widest label plus the
    // padding a face wants each side, so OK does not read as a stub
    // beside Cancel - centered through a zero-height lead slot: the
    // cursor places the slack, same_line rewinds the buttons flush onto
    // it, and a row wider than the content clamps its lead to zero and
    // reads left-packed (the content clip cuts the rest).
    if (button_count > 0) {
        float widest = 0.0f;
        char scratch[128];
        for (int i = 0; i < button_count; ++i) {
            const LabelFit fit = fit_text(stock.labels[i], -1.0f, theme,
                                          scratch, sizeof scratch);
            widest = std::max(widest, fit.advance);
        }
        const float button_width = widest + 2.0f * theme.padding;
        const float row_total =
            static_cast<float>(button_count) * button_width +
            static_cast<float>(button_count - 1) * theme.spacing;
        const float lead =
            std::max(0.0f, layout.remaining_width() - row_total) * 0.5f;
        layout.next(lead, 0.0f);
        layout.same_line(0.0f);
        for (int i = 0; i < button_count; ++i) {
            if (i > 0) {
                layout.same_line();
            }
            // A stock button IS the public button row, nothing
            // private: it registers focusable at the modal stratum (Tab
            // wraps in the dialog), and a click - or the Space a
            // focused one takes - answers with the reading-order index.
            if (button(gui, layout, theme, stock.labels[i], button_width)) {
                open = false;
                event.button = i;
                event.accepted = (i == 0);
            }
        }
    }

    // This frame's placements size the next frame's window - read
    // while the panel is still open (flow_extent speaks about the flow
    // inside it). The open-carry follows `open`: a click that closed
    // the dialog this frame re-arms the Enter guard for the reopen.
    state.extent = layout.flow_extent();
    state.measured = true;
    state.was_open = open;
    end_modal(gui, layout);
    return event;
}

}  // namespace ws::ui::gui
