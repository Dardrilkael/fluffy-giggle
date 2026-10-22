// apps/ui_gallery - one screen with every ws::ui::gui widget family on
// it: the widget arc's acceptance view. A menu bar (Alt mnemonics, a
// submenu, checks, radios, a disabled row with its reason tooltip) and
// a right-click / long-press context menu on the chrome strip; a
// scrolling page of sections - buttons, choices, sliders, text, lists,
// a table, tabs, the overlays (a raw popup, a raw modal scope, the
// five modal-dialog button sets, the toast store), the disabled scope;
// and a status bar the page keeps clear of. The gallery loads no
// assets (its font is the system face, resolved here at the platform
// boundary the same way playground resolves its own), so it builds for
// Windows and Android unchanged: the gui draws through r2d and reads
// one InputFrame, neither of which cares what platform is underneath.
//
// Everything below is caller-owned state driven by immediate-mode
// widget calls - that IS the gui's contract, and GalleryApp's member
// list doubles as the checklist of what a host owns.

#ifdef __ANDROID__
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android_native_app_glue.h>  // android_app + the android_main contract
#include <unistd.h>                   // chdir (imgui.ini's home)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#include <ws/ui/app.hpp>
#include <ws/input/input.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/scoped.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace {

namespace gui = ws::ui::gui;

// The system UI face - the APP picks it, not the ui lib (r2d takes
// caller-resolved paths; font-directory knowledge lives in this TU, the
// platform boundary). One constant per TU, per the house rule. A
// missing face degrades to the fontless v1 bars everywhere, gracefully.
#ifdef __ANDROID__
constexpr const char* kDefaultFontPath = "/system/fonts/Roboto-Regular.ttf";
#else
constexpr const char* kDefaultFontPath = "C:/Windows/Fonts/segoeui.ttf";
#endif

constexpr std::uint32_t kBackdrop = ws::ui::r2d::rgba(22, 24, 30);
constexpr std::uint32_t kGridLine = ws::ui::r2d::rgba(34, 38, 48);
constexpr float kGridStep = 32.0f;

// The flood button's wave - fixed literals because the toast store
// borrows its text (a stack buffer would die with the frame).
const char* const kWave[] = {
    "wave 0", "wave 1", "wave 2", "wave 3", "wave 4",
    "wave 5", "wave 6", "wave 7", "wave 8", "wave 9",
};

// The page's sections, one collapsing header each; the bools are the
// headers' own open state (caller-owned, like everything here).
enum Section {
    kButtons,
    kChoices,
    kSliders,
    kText,
    kLists,
    kTable,
    kTabs,
    kOverlays,
    kDisabled,
    kSectionCount
};

class GalleryApp final : public ws::ui::App {
public:
    explicit GalleryApp(ws::ui::AppConfig config)
        : ws::ui::App(std::move(config)) {}

protected:
    void on_setup() override {
        scale_ = ui_scale();
        font_ = ws::ui::r2d::load_font(kDefaultFontPath, 16.0f * scale_);
        chrome_ = gui::kDefaultChrome.scaled(scale_);
        chrome_.font = font_.empty() ? nullptr : &font_;
        install_clipboard(gui_);  // Ctrl+C/X/V in the text inputs
        section_.fill(true);
    }

    void on_update() override {
        namespace r2d = ws::ui::r2d;

        // The backdrop: the app paints its own background (the shell
        // clears depth, never color); the view menu's grid check draws
        // the reference lines behind the panels.
        r2d::begin();
        const r2d::FrameSize frame = r2d::frame_size();
        r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, kBackdrop);
        if (view_grid_) {
            for (float x = kGridStep; x < frame.width; x += kGridStep) {
                r2d::draw_line(x, 0.0f, x, frame.height, 1.0f, kGridLine);
            }
            for (float y = kGridStep; y < frame.height; y += kGridStep) {
                r2d::draw_line(0.0f, y, frame.width, y, 1.0f, kGridLine);
            }
        }
        r2d::end();

        // The gui block, the playground shape: one screen-space r2d
        // block, the base layer scoped around the page, the toasts
        // drawn after it at their own stratum (50, above the modal
        // veil's 40). The status bar is one control height tall along
        // the bottom edge - that is the toasts' bottom clearance.
        gui_.begin_frame(ws::input::frame_input());
        r2d::begin();
        gui_.begin_layer(gui::Layer::base);
        draw_gallery(frame);
        gui_.end_layer();
        const int toast_fired =
            gui::draw_toasts(gui_, chrome_, toasts_, chrome_.control_height);
        if (toast_fired >= 0) {
            // The gallery's only action toast is "file deleted" ->
            // "undo", so any fired slot is its reply (a real app maps
            // the slot to its meaning - push returned this id).
            toasts_.push({"undone - the file is back",
                          gui::ToastSeverity::info, nullptr, 3.0f});
        }
        r2d::end();
        r2d::set_layer(0);
        gui_.end_frame();
    }

private:
    // Opens the shared dialog as the about box (the menu items and the
    // context menu both land here).
    void open_about() {
        dialog_kind_ = gui::DialogButtons::ok;
        dialog_title_ = "about the gallery";
        dialog_open_ = true;
    }

    // The context menu's reset: everything with a meaningful default
    // goes back to it, so a wandered-off page recovers in one click.
    void reset_counters() {
        clicks_ = 0;
        fraction_ = 0.6f;
        count_ = 16;
        drag_ = 0.0f;
        fruit_ = 0;
        list_sel_ = 0;
        combo_sel_ = 0;
        table_sel_ = 1;
        tab_ = 0;
        lock_check_ = false;
        lock_slide_ = 0.25f;
        toasts_.push({"counters reset", gui::ToastSeverity::info, nullptr,
                      3.0f});
    }

    void draw_gallery(const ws::ui::r2d::FrameSize& frame);

    // ---- the shell seats ----
    gui::GuiContext gui_;
    gui::LayoutCursor layout_;
    gui::ChromeTheme chrome_ = gui::kDefaultChrome;
    ws::ui::r2d::Font font_;
    float scale_ = 1.0f;

    // ---- caller-owned widget state (the gallery IS this list) ----
    std::array<bool, kSectionCount> section_{};
    int clicks_ = 0;
    bool checked_ = false;
    bool dead_check_ = true;  // starts checked: a dimmed-on box reads too
    bool switched_ = true;
    bool dead_switch_ = true;
    int fruit_ = 0;
    float fraction_ = 0.6f;
    int count_ = 16;
    float drag_ = 0.0f;
    char name_buf_[32] = "gallery";
    char locked_buf_[32] = "read only";
    int list_sel_ = 0;
    int combo_sel_ = 0;
    bool combo_open_ = false;
    int table_sel_ = 1;
    int tab_ = 0;
    bool popup_open_ = false;
    gui::Rect popup_point_{};  // where the open popup was triggered
    bool raw_modal_open_ = false;
    bool dialog_open_ = false;
    gui::DialogButtons dialog_kind_ = gui::DialogButtons::ok_cancel;
    const char* dialog_title_ = "a modal dialog";
    char dialog_buf_[24] = "typed in the veil";
    int bar_open_ = -1;  // menu_bar's open menu index (-1 = none)
    bool ctx_open_ = false;
    bool view_grid_ = true;
    bool view_tooltips_ = true;
    int flood_size_ = 6;
    bool ctx_snap_ = true;
    bool lock_ = false;
    bool lock_check_ = false;
    float lock_slide_ = 0.25f;
    gui::Toasts toasts_;
    // The last event's one-line echo (the status bar's tail); every
    // widget reaction above writes here, nothing clears it.
    char status_[96] = "nothing has happened yet";
};

void GalleryApp::draw_gallery(const ws::ui::r2d::FrameSize& frame) {
    // ---- the modal dialog first: the veil-first rule ------------------------
    // The veil is frame-wide the frame it rises, so the call must run
    // before the widgets it blocks. One call site plays every standard
    // button set (the section's buttons pick `kind_`); the body shows
    // the dialog's own cursor and id scope - a live text input the
    // veil traps Tab inside - and the none set's one way out, its own
    // button (a none dialog takes no Enter and no Escape at all).
    if (dialog_open_) {
        const gui::DialogEvent ev = gui::modal_dialog(
            gui_, chrome_, "gallery dialog", dialog_title_, dialog_kind_,
            dialog_open_, 300.0f * scale_,
            [this](gui::GuiContext& g, gui::LayoutCursor& l,
                   const gui::ChromeTheme& t) {
                gui::text(l, t, "the veil blocks the page below;", -1.0f);
                gui::text(l, t, "Tab stays trapped in here, and a toast",
                          -1.0f);
                gui::text(l, t, "still rides above it (50 over 40)", -1.0f);
                gui::text_input(g, l, t, "name", dialog_buf_,
                                static_cast<int>(sizeof dialog_buf_), -1.0f);
                if (dialog_kind_ == gui::DialogButtons::none) {
                    if (gui::button(g, l, t, "close this dialog", -1.0f)) {
                        dialog_open_ = false;
                    }
                }
            });
        if (ev.button >= 0) {
            std::snprintf(status_, sizeof status_, "dialog: %s",
                          ev.accepted ? "accepted" : "dismissed");
            toasts_.push({ev.accepted ? "the dialog was accepted"
                                      : "the dialog was dismissed",
                          ev.accepted ? gui::ToastSeverity::info
                                      : gui::ToastSeverity::warning,
                          nullptr, 3.0f});
        }
    }

    // ---- the chrome strip: menu bar + the context-menu target ---------------
    // One panel holds both rows: the bar's titles flow left along the
    // top, and the strip as a whole is the context menu's region -
    // right-click it (press and hold on touch) and the menu opens at
    // the pointer. The item arrays are per-frame stack data, the menus'
    // discipline: re-described every frame, what fires comes back as a
    // pointer into this frame's own arrays.
    const float strip_h = chrome_.padding * 2.0f +
                          chrome_.control_height * 2.0f + chrome_.spacing;
    const gui::Rect strip{0.0f, 0.0f, frame.width, strip_h};
    const gui::Rect strip_content =
        gui::begin_panel(gui_, layout_, chrome_, "gallery bar", strip);

    const gui::MenuItem recent[] = {{"alpha.txt"}, {"beta.txt"}};
    const gui::MenuItem demo_items[] = {
        {"&new", {}, "Ctrl+N"},
        {"open...", {}, "Ctrl+O"},
        {.kind = gui::MenuItemKind::separator},
        {"recent", {}, nullptr, recent, 2},
        {.kind = gui::MenuItemKind::separator},
        {"&about the gallery", {}, "F1"},
    };
    const gui::MenuItem view_items[] = {
        {.label = "grid backdrop", .kind = gui::MenuItemKind::check,
         .checked = view_grid_},
        {.label = "tooltips", .kind = gui::MenuItemKind::check,
         .checked = view_tooltips_},
        {.kind = gui::MenuItemKind::separator},
        {.label = "calm floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 3},
        {.label = "lively floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 6},
        {.label = "storm floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 10},
    };
    const gui::MenuItem help_items[] = {
        {"&about", {}, "F1"},
        {.label = "docs", .shortcut = "F2", .disabled = true,
         .disabled_reason = "not shipped yet"},
    };
    const gui::MenuDesc bar[] = {
        {"&demo", demo_items, 6},
        {"&view", view_items, 7},
        {"&help", help_items, 2},
    };
    const gui::MenuEvent ev = gui::menu_bar(gui_, layout_, chrome_, "the bar",
                                            bar, 3, bar_open_);
    if (ev.item != nullptr) {
        // Identity against this frame's own arrays is the whole wiring;
        // checks and radios flip caller state (the rows show it next
        // frame) and the rest speak through toasts.
        if (ev.item == &demo_items[0]) {
            toasts_.push({"nothing to save yet", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &demo_items[1]) {
            toasts_.push({"the gallery owns no files",
                          gui::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &recent[0]) {
            toasts_.push({"opened alpha.txt", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &recent[1]) {
            toasts_.push({"opened beta.txt", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &demo_items[5] || ev.item == &help_items[0]) {
            open_about();
        } else if (ev.item == &view_items[0]) {
            view_grid_ = !view_grid_;
        } else if (ev.item == &view_items[1]) {
            view_tooltips_ = !view_tooltips_;
        } else if (ev.item == &view_items[3]) {
            flood_size_ = 3;
        } else if (ev.item == &view_items[4]) {
            flood_size_ = 6;
        } else if (ev.item == &view_items[5]) {
            flood_size_ = 10;
        }
    }
    // The strip's second row, truncated to the content width so a
    // narrow phone clips the hint instead of spilling past the chrome.
    gui::text(layout_, chrome_,
              "right-click (or press and hold) this strip - a context menu "
              "opens at the pointer",
              strip_content.width);
    gui::end_panel(gui_, layout_);

    const gui::MenuItem ctx_items[] = {
        {"reset the counters", {}, "Ctrl+R"},
        {.label = "snap to grid", .kind = gui::MenuItemKind::check,
         .checked = ctx_snap_},
        {.kind = gui::MenuItemKind::separator},
        {"about the gallery", {}, "F1"},
        {.label = "delete the gallery", .shortcut = "Del",
         .disabled = true, .disabled_reason = "there is nothing to delete"},
    };
    const gui::MenuEvent ctx = gui::context_menu(gui_, layout_, chrome_,
                                                 "gallery ctx", strip,
                                                 ctx_items, 5, ctx_open_);
    if (ctx.item != nullptr) {
        if (ctx.item == &ctx_items[0]) {
            reset_counters();
        } else if (ctx.item == &ctx_items[1]) {
            ctx_snap_ = !ctx_snap_;
        } else if (ctx.item == &ctx_items[3]) {
            open_about();
        }
    }

    // ---- the page: a scroll panel of sections -------------------------------
    // The scroll panel is itself a gallery piece (wheel + touch drag,
    // the persistent offset), and it gives the sections room on a
    // phone. It stops short of the status bar along the bottom edge.
    const float status_h = chrome_.control_height;
    const gui::Rect page_bounds{0.0f, strip_h, frame.width,
                                std::max(0.0f, frame.height - strip_h -
                                                   status_h)};
    const gui::Rect page = gui::begin_scroll_panel(gui_, layout_, chrome_,
                                                   "gallery page", page_bounds);
    const float half = (page.width - chrome_.spacing) * 0.5f;

    // --- buttons -----------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "buttons",
                               section_[kButtons])) {
        if (gui::button(gui_, layout_, chrome_, "click me", half)) {
            ++clicks_;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "hover me (tooltip)", half)) {
            ++clicks_;
        }
        if (view_tooltips_) {
            // The one-line form: rest delay, placement and sizing folded
            // into one call, keyed on the same label the button was
            // laid under and anchored at the pointer (a point anchor -
            // the hint hangs below it).
            gui::tooltip_text(
                gui_, layout_, chrome_, "hover me (tooltip)",
                {gui_.input().mouse_x, gui_.input().mouse_y, 0.0f, 0.0f},
                "a one-line hint, delayed and placed for you");
        }
        if (gui::button(gui_, layout_, chrome_, "a dead button", half, {},
                        true)) {
            ++clicks_;  // the disabled row can never report
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "a fill-width row", -1.0f)) {
            ++clicks_;
        }
        if (gui::button(gui_, layout_, chrome_,
                        "hover me too - a two-row tooltip", -1.0f)) {
            ++clicks_;
        }
        if (view_tooltips_) {
            // The scope form: the delay gate plus a scope that holds
            // any widgets - two text rows here, on the pointer-following
            // bounds playground's hint uses.
            const gui::WidgetId rich_id =
                gui_.ids().id("hover me too - a two-row tooltip");
            if (gui::tooltip_delayed(gui_, rich_id, chrome_)) {
                const float tip_h = chrome_.control_height * 2.0f +
                                   chrome_.spacing +
                                   chrome_.padding * 2.0f;
                const gui::Rect tip_bounds{gui_.input().mouse_x +
                                               14.0f * scale_,
                                           gui_.input().mouse_y +
                                               18.0f * scale_,
                                           260.0f * scale_, tip_h};
                if (gui::begin_tooltip(gui_, layout_, chrome_, "rich tip",
                                       tip_bounds, true)) {
                    gui::text(layout_, chrome_,
                              "the scope form holds any widgets -", -1.0f);
                    gui::text(layout_, chrome_,
                              "two rows here, a button would work too",
                              -1.0f);
                    gui::end_tooltip(gui_, layout_);
                }
            }
        }
        char clicks_line[48];
        std::snprintf(clicks_line, sizeof clicks_line, "clicks so far: %d",
                      clicks_);
        gui::text(layout_, chrome_, clicks_line, -1.0f);
    }

    // --- choices ------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "choices",
                               section_[kChoices])) {
        if (gui::checkbox(gui_, layout_, chrome_, "a checkbox", checked_)) {
            // the widget already flipped the bool; the branch is the
            // app's own reaction slot
        }
        layout_.same_line();
        (void)gui::checkbox(gui_, layout_, chrome_, "a dead checkbox",
                           dead_check_, true);
        if (gui::toggle_switch(gui_, layout_, chrome_, "a toggle switch",
                               switched_)) {
        }
        layout_.same_line();
        (void)gui::toggle_switch(gui_, layout_, chrome_, "a dead switch",
                                 dead_switch_, true);
        gui::text(layout_, chrome_,
                  "pick a fruit - one shared int is the whole radio group:",
                  -1.0f);
        const char* const fruits[] = {"apple", "banana", "cherry"};
        for (int i = 0; i < 3; ++i) {
            if (gui::radio(gui_, layout_, chrome_, fruits[i], fruit_, i)) {
                std::snprintf(status_, sizeof status_, "fruit: %s", fruits[i]);
            }
            if (i < 2) {
                layout_.same_line();
            }
        }
    }

    // --- sliders & drag ------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "sliders & drag",
                               section_[kSliders])) {
        if (gui::slider_float(gui_, layout_, chrome_, "fill", fraction_, 0.0f,
                              1.0f, half)) {
        }
        layout_.same_line();
        (void)gui::slider_float(gui_, layout_, chrome_, "dead", fraction_,
                                0.0f, 1.0f, half, true);
        if (gui::slider_int(gui_, layout_, chrome_, "count", count_, 0, 100,
                            half)) {
        }
        layout_.same_line();
        if (gui::drag_float(gui_, layout_, chrome_, "drag", drag_, -10.0f,
                            10.0f, 0.05f, half)) {
        }
        gui::text(layout_, chrome_,
                  "the fill slider feeds the progress bar below it:", -1.0f);
        gui::progress(layout_, chrome_, fraction_, -1.0f);
    }

    // --- text ------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "text",
                               section_[kText])) {
        if (gui::text_input(gui_, layout_, chrome_, "name", name_buf_,
                            static_cast<int>(sizeof name_buf_), half)) {
            // true on the frame the TEXT changed; caret and selection
            // moves answer false
            std::snprintf(status_, sizeof status_, "typed: %s", name_buf_);
        }
        layout_.same_line();
        (void)gui::text_input(gui_, layout_, chrome_, "locked", locked_buf_,
                              static_cast<int>(sizeof locked_buf_), half,
                              true);
        gui::text(layout_, chrome_,
                  "a static label - no id, no hit-test, nothing focusable",
                  -1.0f);
        // Enter is not the input's own: the app reads the focus plus the
        // key edge and gives the field its commit semantics.
        const gui::WidgetId name_id = gui_.ids().id("name");
        if (gui_.is_focused(name_id) &&
            ws::input::frame_input().key_pressed[ws::input::keys::kEnter]) {
            std::snprintf(status_, sizeof status_, "committed: %s",
                          name_buf_);
        }
        gui::separator(layout_, chrome_);
        gui::text(layout_, chrome_, "a separator above", -1.0f);
    }

    // --- lists ------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "lists",
                               section_[kLists])) {
        gui::text(layout_, chrome_,
                  "selectable rows - the selection stays the caller's:",
                  -1.0f);
        const char* const rows[] = {"first row", "second row", "third row"};
        for (int i = 0; i < 3; ++i) {
            if (gui::selectable(gui_, layout_, chrome_, rows[i],
                                list_sel_ == i)) {
                list_sel_ = i;
            }
        }
        gui::text(layout_, chrome_,
                  "a combo - the dropdown is a popup the widget places:",
                  -1.0f);
        const char* const picks[] = {"red", "green", "blue", "black"};
        if (gui::combo(gui_, layout_, chrome_, "color", picks, 4, combo_sel_,
                       combo_open_, -1.0f)) {
            std::snprintf(status_, sizeof status_, "color: %s",
                          picks[combo_sel_]);
        }
    }

    // --- table -------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "table",
                               section_[kTable])) {
        const gui::TableColumn cols[] = {
            {"name", 0.0f},  // stretch: fixed columns take theirs first
            {"size", 72.0f},
            {"modified", 96.0f},
        };
        const char* const cells[] = {
            "notes.txt",  "2 kB",   "today",
            "shot.png",   "140 kB", "yesterday",
            "wallet.dat", "1 MB",   "last year",
            "todo.md",    "3 kB",   "this morning",
        };
        (void)gui::table(gui_, layout_, chrome_, "files", cols, 3, cells, 4,
                         &table_sel_);
        char sel_line[48];
        std::snprintf(sel_line, sizeof sel_line, "selected row: %d",
                      table_sel_);
        gui::text(layout_, chrome_, sel_line, -1.0f);
    }

    // --- tabs & the statics they hold --------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "tabs & statics",
                               section_[kTabs])) {
        const char* const tabs[] = {"spinner", "busy bar", "a panel"};
        (void)gui::tab_bar(gui_, layout_, chrome_, "pages", tabs, 3, tab_,
                           page.width);
        if (tab_ == 0) {
            // The spinner's label ids it and never draws - the row is
            // the dots alone, eight of them chasing on the frame clock.
            gui::spinner(gui_, layout_, chrome_, "spin");
            layout_.same_line();
            gui::text(layout_, chrome_, "eight dots chasing on the frame clock",
                      -1.0f);
        } else if (tab_ == 1) {
            gui::progress_indeterminate(gui_, layout_, chrome_, "busy",
                                         -1.0f);
        } else {
            // A nested panel: chrome + clip + id scope, the outer cursor
            // parked while it is open.
            const gui::Rect nested_bounds =
                layout_.next(page.width * 0.6f,
                             chrome_.control_height * 2.0f + chrome_.padding);
            const gui::Rect nested = gui::begin_panel(gui_, layout_, chrome_,
                                                      "nested", nested_bounds);
            gui::text(layout_, chrome_, "a panel inside the page", -1.0f);
            if (gui::button(gui_, layout_, chrome_, "inside", nested.width)) {
                ++clicks_;
            }
            gui::end_panel(gui_, layout_);
        }
    }

    // --- overlays: popup, modal, dialogs, toasts ----------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_,
                               "overlays: popup, modal, dialogs, toasts",
                               section_[kOverlays])) {
        // The raw popup: the caller owns `open`, places the window and
        // lays the rows. The anchor is the press point captured when
        // the button opened it - the context menu's own anchoring rule
        // (a point anchor: the window hangs below it).
        if (gui::button(gui_, layout_, chrome_, "open a popup", half)) {
            popup_open_ = !popup_open_;
            popup_point_ = gui::Rect{gui_.input().mouse_x,
                                     gui_.input().mouse_y, 0.0f, 0.0f};
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "a raw modal scope", half)) {
            raw_modal_open_ = !raw_modal_open_;  // begin_modal reads it BY VALUE
        }
        if (popup_open_) {
            const gui::Rect pop_bounds = gui::place_popup(
                popup_point_, 170.0f,
                chrome_.control_height * 3.0f + chrome_.spacing * 2.0f +
                    chrome_.padding * 2.0f);
            // The guard form (scoped.hpp): the same begin/end pair as
            // one block local, so a return from inside the rows cannot
            // strand the scope.
            const gui::ScopedPopup pop(gui_, layout_, chrome_,
                                       "gallery popup", popup_point_,
                                       pop_bounds, popup_open_);
            if (pop) {
                const char* const popup_rows[] = {"toast an info",
                                                  "toast a warning",
                                                  "just close"};
                for (int i = 0; i < 3; ++i) {
                    if (gui::selectable(gui_, layout_, chrome_, popup_rows[i],
                                        false)) {
                        if (i < 2) {
                            toasts_.push({i == 0 ? "a popup said hello"
                                                 : "a popup said watch out",
                                          i == 0
                                              ? gui::ToastSeverity::info
                                              : gui::ToastSeverity::warning,
                                          nullptr, 3.0f});
                        }
                        popup_open_ = false;
                    }
                }
            }  // end_popup rides the guard
        }
        if (raw_modal_open_) {
            const float mw = 280.0f * scale_;
            const float mh = 120.0f * scale_;
            const gui::Rect modal_bounds{(frame.width - mw) * 0.5f,
                                         (frame.height - mh) * 0.5f, mw, mh};
            if (gui::begin_modal(gui_, layout_, chrome_, "raw modal",
                                 modal_bounds, raw_modal_open_)) {
                gui::text(layout_, chrome_,
                          "the raw modal scope: the caller places", -1.0f);
                gui::text(layout_, chrome_,
                          "it, draws its rows and closes it", -1.0f);
                if (gui::button(gui_, layout_, chrome_, "close", -1.0f)) {
                    raw_modal_open_ = false;
                }
                gui::end_modal(gui_, layout_);
            }
        }

        // The stock dialog sets, one button each: the one call site at
        // the top of the frame plays whatever kind these pick (the
        // about-box entries reuse it too). Stock sets clear `open`
        // themselves - Esc counts as a dismiss, Enter as an accept once
        // the veil has settled; the none set takes neither and has no
        // way out but its own body button.
        if (gui::button(gui_, layout_, chrome_, "ok only", half)) {
            dialog_kind_ = gui::DialogButtons::ok;
            dialog_title_ = "an ok-only dialog";
            dialog_open_ = true;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "ok + cancel", half)) {
            dialog_kind_ = gui::DialogButtons::ok_cancel;
            dialog_title_ = "a modal dialog";
            dialog_open_ = true;
        }
        if (gui::button(gui_, layout_, chrome_, "yes + no", half)) {
            dialog_kind_ = gui::DialogButtons::yes_no;
            dialog_title_ = "a yes/no question";
            dialog_open_ = true;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "retry + cancel", half)) {
            dialog_kind_ = gui::DialogButtons::retry_cancel;
            dialog_title_ = "something failed";
            dialog_open_ = true;
        }
        if (gui::button(gui_, layout_, chrome_, "no buttons at all", -1.0f)) {
            dialog_kind_ = gui::DialogButtons::none;
            dialog_title_ = "a dialog with no buttons";
            dialog_open_ = true;
        }

        // The toast store's contracts, one button each: the three
        // severities, an action button (its click dismisses the toast
        // and reports the slot - on_update answers it), a sticky toast
        // (duration 0: only a click removes it), the flood policy (the
        // view menu sizes the wave) and clear. The count reads live.
        // The texts are literals on purpose - a toast borrows its bytes
        // and re-reads them every frame it draws.
        if (gui::button(gui_, layout_, chrome_, "info", half)) {
            toasts_.push({"saved the layout", gui::ToastSeverity::info,
                          nullptr, 4.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "warn", half)) {
            toasts_.push({"the disk is almost full",
                          gui::ToastSeverity::warning, nullptr, 4.0f});
        }
        if (gui::button(gui_, layout_, chrome_, "error", half)) {
            toasts_.push({"lost the connection", gui::ToastSeverity::error,
                          nullptr, 4.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "action", half)) {
            toasts_.push({"file deleted", gui::ToastSeverity::info, "undo",
                          5.0f});
        }
        if (gui::button(gui_, layout_, chrome_, "sticky", half)) {
            toasts_.push({"this one stays until clicked",
                          gui::ToastSeverity::info, nullptr, 0.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "flood", half)) {
            for (int i = 0; i < flood_size_; ++i) {
                toasts_.push({kWave[i], gui::ToastSeverity::info, nullptr,
                              1.5f});
            }
        }
        if (gui::button(gui_, layout_, chrome_, "clear", half)) {
            toasts_.clear();
        }
        layout_.same_line();
        char live[32];
        std::snprintf(live, sizeof live, "live: %d", toasts_.count());
        gui::text(layout_, chrome_, live, half);
    }

    // --- the disabled scope ------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "the disabled scope",
                               section_[kDisabled])) {
        gui::text(layout_, chrome_,
                  "one begin_disabled greys everything inside it and",
                  -1.0f);
        gui::text(layout_, chrome_,
                  "refuses it the pointer, the wheel and the Tab walk:",
                  -1.0f);
        (void)gui::checkbox(gui_, layout_, chrome_, "lock the rows below",
                            lock_);
        gui_.begin_disabled(lock_);
        if (gui::button(gui_, layout_, chrome_, "a locked button", half)) {
            ++clicks_;  // unreachable while the lock holds
        }
        layout_.same_line();
        (void)gui::checkbox(gui_, layout_, chrome_, "a locked checkbox",
                            lock_check_);
        if (gui::slider_float(gui_, layout_, chrome_, "a locked slider",
                              lock_slide_, 0.0f, 1.0f, -1.0f)) {
        }
        gui_.end_disabled();
    }

    gui::end_scroll_panel(gui_, layout_, chrome_);

    // The status bar: the one static that places itself - full width,
    // one control height along the bottom edge (the page was sized to
    // leave it that room, and it is the toasts' bottom clearance in
    // on_update). The tail is the last event's echo; the head recomposes
    // every frame into a local buffer, safe because the bar consumes
    // the bytes this frame.
    char line[160];
    std::snprintf(line, sizeof line,
                  "ws ui gallery - %d clicks - %d toasts - grid %s | %s",
                  clicks_, toasts_.count(), view_grid_ ? "on" : "off",
                  status_);
    (void)gui::status_bar(chrome_, line);
}

}  // namespace

#ifdef __ANDROID__
extern "C" void android_main(android_app* app) {
    chdir(app->activity->internalDataPath);
    ws::ui::AppConfig config;
    config.app_handle = app;
    GalleryApp gallery(std::move(config));
    (void)gallery.run();
}
#else
int main() {
    ws::ui::AppConfig config;
    config.title = "ws ui gallery";
    config.width = 1000;
    config.height = 700;
    GalleryApp gallery(std::move(config));
    return gallery.run();
}
#endif
