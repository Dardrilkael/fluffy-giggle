// MenuDemo's panel: the item arrays are plain per-frame stack data (the
// menus' discipline - they are re-described every frame, and what fires
// comes back as a pointer into them), so the wiring below is identity
// against this frame's own arrays. The toast texts are string literals on
// purpose: a Toast borrows its bytes and re-reads them every frame it
// draws, so nothing here may hand it a buffer that dies with the frame.

#include "menu_demo.hpp"

#include <cstdio>

namespace playground {
namespace {

// The flood button's wave - fixed literals because the toast store
// borrows its text (a stack buffer would die with the frame).
const char* const kWave[] = {
    "wave 0", "wave 1", "wave 2", "wave 3", "wave 4",
    "wave 5", "wave 6", "wave 7", "wave 8", "wave 9",
};

}  // namespace

void MenuDemo::draw_panel(ws::ui::gui::GuiContext& gui,
                          ws::ui::gui::LayoutCursor& layout,
                          const ws::ui::gui::ChromeTheme& theme,
                          const ws::ui::gui::Rect& bounds) {
    namespace gui_ns = ws::ui::gui;

    // The dialog first, the veil-first rule: modal chrome sorts above
    // base wherever it records, but the veil must block this very panel
    // on the frame it opens - and a toast pushed from behind it stays
    // clickable, the toast stratum sitting above the veil (50 > 40).
    if (dialog_open_) {
        const gui_ns::DialogEvent ev = gui_ns::modal_dialog(
            gui, theme, "demo dialog", "a modal dialog",
            gui_ns::DialogButtons::ok_cancel, dialog_open_, 280.0f,
            [this](gui_ns::GuiContext& g, gui_ns::LayoutCursor& l,
                   const gui_ns::ChromeTheme& t) {
                gui_ns::text(l, t, "the veil blocks the world below;", -1.0f);
                gui_ns::text(l, t, "a toast still rides above it", -1.0f);
                if (gui_ns::button(g, l, t, "toast from the veil", -1.0f)) {
                    toasts_.push({"hello from behind the veil",
                                  gui_ns::ToastSeverity::info, nullptr,
                                  3.0f});
                }
            });
        if (ev.button >= 0) {
            toasts_.push({ev.accepted ? "the dialog was accepted"
                                      : "the dialog was cancelled",
                          ev.accepted ? gui_ns::ToastSeverity::info
                                      : gui_ns::ToastSeverity::warning,
                          nullptr, 3.0f});
        }
    }

    const gui_ns::Rect content =
        gui_ns::begin_panel(gui, layout, theme, "menu demo", bounds);

    // --- the bar ------------------------------------------------------------
    // Titles carry '&' mnemonics (Alt+F/E/V from anywhere on the bar);
    // the items demonstrate one of each row shape: actions with
    // shortcuts, a parent with a submenu, checks, radios, separators and
    // a disabled row whose rest shows its reason.
    const gui_ns::MenuItem recent[] = {
        {"alpha.txt"},
        {"beta.txt"},
    };
    const gui_ns::MenuItem file_items[] = {
        {"&new", {}, "Ctrl+N"},
        {"open...", {}, "Ctrl+O"},
        {.kind = gui_ns::MenuItemKind::separator},
        {"recent", {}, nullptr, recent, 2},
        {.kind = gui_ns::MenuItemKind::separator},
        {"quit", {}, "Ctrl+Q"},
    };
    const gui_ns::MenuItem edit_items[] = {
        {.label = "undo", .shortcut = "Ctrl+Z", .disabled = true,
         .disabled_reason = "nothing to undo yet"},
        {.kind = gui_ns::MenuItemKind::separator},
        {"cut", {}, "Ctrl+X"},
        {"copy", {}, "Ctrl+C"},
        {"paste", {}, "Ctrl+V"},
    };
    const gui_ns::MenuItem view_items[] = {
        {.label = "grid lines", .kind = gui_ns::MenuItemKind::check,
         .checked = view_grid_},
        {.label = "debug overlay", .kind = gui_ns::MenuItemKind::check,
         .checked = view_debug_},
        {.kind = gui_ns::MenuItemKind::separator},
        {.label = "low detail", .kind = gui_ns::MenuItemKind::radio,
         .checked = detail_ == 0},
        {.label = "medium detail", .kind = gui_ns::MenuItemKind::radio,
         .checked = detail_ == 1},
        {.label = "high detail", .kind = gui_ns::MenuItemKind::radio,
         .checked = detail_ == 2},
        {.kind = gui_ns::MenuItemKind::separator},
        {.label = "custom face", .disabled = true,
         .disabled_reason = "not in the demo build"},
    };
    const gui_ns::MenuDesc bar[] = {
        {"&file", file_items, 6},
        {"&edit", edit_items, 5},
        {"&view", view_items, 8},
    };
    const gui_ns::MenuEvent ev =
        gui_ns::menu_bar(gui, layout, theme, "menus", bar, 3, bar_open_);
    if (ev.item != nullptr) {
        // Identity against this frame's own arrays is the whole wiring:
        // the event carries the app's bytes, the app supplies the
        // meaning. Checks and radios flip caller state (the rows show
        // it next frame); separators never fire and disabled rows
        // cannot.
        if (ev.item == &file_items[0]) {
            toasts_.push({"new file: nothing to save yet",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &file_items[1]) {
            toasts_.push({"open: the demo owns no files",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &recent[0]) {
            toasts_.push({"opened alpha.txt",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &recent[1]) {
            toasts_.push({"opened beta.txt",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &file_items[5]) {
            toasts_.push({"quit: the demo keeps running",
                          gui_ns::ToastSeverity::warning, nullptr, 3.0f});
        } else if (ev.item == &edit_items[2]) {
            toasts_.push({"cut: nothing selected",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &edit_items[3]) {
            toasts_.push({"copied (not really)",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &edit_items[4]) {
            toasts_.push({"pasted into the void",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &view_items[0]) {
            view_grid_ = !view_grid_;
        } else if (ev.item == &view_items[1]) {
            view_debug_ = !view_debug_;
        } else if (ev.item >= &view_items[3] && ev.item <= &view_items[5]) {
            detail_ = static_cast<int>(ev.item - &view_items[3]);
        }
    }

    // --- the context target --------------------------------------------------
    // Right-click (or a touch held past the long-press time) inside the
    // box opens the menu at the press point; a second right-click while
    // open moves it. The box itself is a plain sub-panel - the context
    // menu anchors to its bounds, not to any widget.
    gui_ns::text(layout, theme, "right-click the box below:", -1.0f);
    const gui_ns::Rect ctx_bounds =
        layout.next(content.width, theme.control_height * 3.0f);
    {
        const gui_ns::Rect ctx_content = gui_ns::begin_panel(
            gui, layout, theme, "ctx target", ctx_bounds);
        gui_ns::text(layout, theme, "a menu opens at the press point",
                     -1.0f);
        gui_ns::text(layout, theme, "touch: press and hold", -1.0f);
        (void)ctx_content;
        gui_ns::end_panel(gui, layout);
    }
    const gui_ns::MenuItem ctx_send[] = {
        {"notes"},
        {"trash"},
    };
    const gui_ns::MenuItem ctx_items[] = {
        {"duplicate", {}, "Ctrl+D"},
        {.label = "snap to grid", .kind = gui_ns::MenuItemKind::check,
         .checked = ctx_snap_},
        {.kind = gui_ns::MenuItemKind::separator},
        {"send to", {}, nullptr, ctx_send, 2},
        {.kind = gui_ns::MenuItemKind::separator},
        {.label = "delete", .shortcut = "Del", .disabled = true,
         .disabled_reason = "nothing is selected"},
    };
    const gui_ns::MenuEvent ctx_ev = gui_ns::context_menu(
        gui, layout, theme, "ctx", ctx_bounds, ctx_items, 6, ctx_open_);
    if (ctx_ev.item != nullptr) {
        if (ctx_ev.item == &ctx_items[0]) {
            toasts_.push({"duplicated the nothing",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ctx_ev.item == &ctx_items[1]) {
            ctx_snap_ = !ctx_snap_;
        } else if (ctx_ev.item == &ctx_send[0]) {
            toasts_.push({"sent to notes",
                          gui_ns::ToastSeverity::info, nullptr, 3.0f});
        } else if (ctx_ev.item == &ctx_send[1]) {
            toasts_.push({"sent to trash",
                          gui_ns::ToastSeverity::error, nullptr, 3.0f});
        }
    }

    // --- the toast buttons -----------------------------------------------------
    // The store's contracts one button each: the three severities, an
    // action button (its click dismisses and reports the slot), a sticky
    // toast (duration 0: only a click removes it), the flood policy (ten
    // pushes past the eight-slot cap drop the oldest, warned once), and
    // clear. The count reads live.
    const float half = (content.width - theme.spacing) * 0.5f;
    if (gui_ns::button(gui, layout, theme, "info", half)) {
        toasts_.push({"saved the layout", gui_ns::ToastSeverity::info,
                      nullptr, 4.0f});
    }
    layout.same_line();
    if (gui_ns::button(gui, layout, theme, "warn", half)) {
        toasts_.push({"the disk is almost full",
                      gui_ns::ToastSeverity::warning, nullptr, 4.0f});
    }
    if (gui_ns::button(gui, layout, theme, "error", half)) {
        toasts_.push({"lost the connection", gui_ns::ToastSeverity::error,
                      nullptr, 4.0f});
    }
    layout.same_line();
    if (gui_ns::button(gui, layout, theme, "action", half)) {
        toasts_.push({"file deleted", gui_ns::ToastSeverity::info, "undo",
                      5.0f});
    }
    if (gui_ns::button(gui, layout, theme, "sticky", half)) {
        toasts_.push({"this one stays until clicked",
                      gui_ns::ToastSeverity::info, nullptr, 0.0f});
    }
    layout.same_line();
    if (gui_ns::button(gui, layout, theme, "flood", half)) {
        for (int i = 0; i < 10; ++i) {
            toasts_.push({kWave[static_cast<std::size_t>(i)],
                          gui_ns::ToastSeverity::info, nullptr, 1.5f});
        }
    }
    if (gui_ns::button(gui, layout, theme, "clear", half)) {
        toasts_.clear();
    }
    layout.same_line();
    char live[32];
    std::snprintf(live, sizeof live, "live: %d", toasts_.count());
    gui_ns::text(layout, theme, live, half);

    // --- the dialog opener (drawn first above, flipped open here) --------------
    if (gui_ns::button(gui, layout, theme, "open a modal dialog", -1.0f)) {
        dialog_open_ = true;
    }

    gui_ns::end_panel(gui, layout);
}

void MenuDemo::toast_fired(int slot) {
    // The demo's only action toast is "file deleted" -> "undo", so any
    // fired slot is its reply (a real app maps the slot to its meaning -
    // push returned this id when the toast was queued, and the fired
    // toast is already dismissed by the time this runs).
    (void)slot;
    toasts_.push({"undone - the file is back",
                  ws::ui::gui::ToastSeverity::info, nullptr, 3.0f});
}

}  // namespace playground
