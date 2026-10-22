// The menu + toast demo screen: the gui widget arc's tail, end to end.
// A menu bar (Alt mnemonics, a submenu chain, checks, radios, a disabled
// row with its reason tooltip), a right-click / long-press context
// target, and the toast store's whole story - the three severities, an
// action button, a sticky toast, the flood policy, a live count. The
// toast DRAW is main.cpp's: draw_toasts runs once a frame at the toast
// stratum (above the modal veil, clear of the sound panel), and the slot
// whose action fired goes to toast_fired().
#pragma once

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>

namespace playground {

class MenuDemo {
public:
    // The panel (call inside the gui block, once a frame).
    void draw_panel(ws::ui::gui::GuiContext& gui,
                    ws::ui::gui::LayoutCursor& layout,
                    const ws::ui::gui::ChromeTheme& theme,
                    const ws::ui::gui::Rect& bounds);

    // The store the panel's buttons feed; main.cpp draws it once a frame.
    ws::ui::gui::Toasts& toasts() { return toasts_; }

    // draw_toasts' reply: an action button fired on this slot. The
    // demo's one action toast ("file deleted" -> "undo") answers with a
    // follow-up - the push/fired round trip in two lines of app code.
    void toast_fired(int slot);

private:
    // All caller-owned, per the contracts: the menus only report.
    int bar_open_ = -1;        // menu_bar's open menu index
    bool ctx_open_ = false;    // context_menu's open bool
    bool dialog_open_ = false;
    // The view menu's display state (the menu fires; the app flips).
    bool view_grid_ = true;
    bool view_debug_ = false;
    int detail_ = 1;           // the radio trio's selection
    // The context menu's display state.
    bool ctx_snap_ = true;

    ws::ui::gui::Toasts toasts_;
};

}  // namespace playground
