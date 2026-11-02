#pragma once

// The skinnable library's painter seam (the ui-skin plan's P3): one
// table of plain function pointers, one row per widget draw half, and
// a theme-carried pointer to it. The stock look is nothing special -
// it is kStockPainters, the very same draws the composed widgets
// always called; a skin replaces any subset of rows and every
// widget that draws through that row follows, with no fork of the
// library and no rebuild of its behavior.
//
// THE CONTRACT (a painter's side of the seam):
// - A painter RECEIVES the behavior's answer - the paint-ready result
//   (state/focus/hit folded), the placed rect, the theme and the
//   row's own data - and it draws ONLY. No GuiContext, no state
//   change, no allocation, no time. The interaction is never the
//   painter's to re-run; identity, focus and the click belong to the
//   behavior half, which never reads this table.
// - A painter may read any token (including painters of its own
//   sub-rows) and may COMPOSE: a skin's row may call the stock draw
//   (kStockPainters names them all) and paint over it - composition,
//   not inheritance. There are no virtuals, no std::function, no
//   capture: a row is a plain function, stateless by construction.
// - Signatures are frozen exactly as the public *_draw functions
//   spell them; the table adds nothing, reorders nothing, defaults
//   nothing (a row's default arguments belong to the widget call,
//   not the function type - pass {} explicitly through a pointer).
// - A null row inside an installed table falls back to that row's
//   stock draw, so a partial table is legal; the easy full-skin
//   spelling is copy-then-override:
//
//       ws::ui::gui::WidgetPainters skin = ws::ui::gui::kStockPainters;
//       skin.button = my_button;          // one row, my look
//       theme.painters = &skin;           // the theme points; it owns
//                                          // nothing (the font rule)
//
// The table's lifetime is the caller's: a theme carrying a painters
// pointer must not outlive the table (or the fonts it borrows). The
// statics (text, separator, progress, status_bar) are deliberately
// NOT rows: they are chrome, not citizens, and a skin recomposes them
// from the primitives it gets here.

#include <ws/ui/gui/widgets.hpp>

namespace ws::ui::gui {

// One row per widget draw half - the same signatures widgets.hpp
// declares, grouped the way a skin reads them: the press-and-choose
// rows, the sliders, the chrome rows, the overlay rows, the data
// rows.
struct WidgetPainters {
    // --- the press-and-choose rows -------------------------------------
    void (*button)(const Rect&, const ButtonResult&, const ChromeTheme&,
                   const char*, r2d::Sprite);
    void (*checkbox)(const Rect&, const CheckboxResult&, const ChromeTheme&,
                     const char*, bool);
    void (*radio)(const Rect&, const RadioResult&, const ChromeTheme&,
                  const char*, bool);
    void (*toggle_switch)(const Rect&, const ToggleSwitchResult&,
                          const ChromeTheme&, const char*, bool);
    void (*collapsing_header)(const Rect&, const CollapsingHeaderResult&,
                              const ChromeTheme&, const char*, bool);
    void (*selectable)(const Rect&, const SelectableResult&,
                       const ChromeTheme&, const char*, bool);

    // --- the slider rows -----------------------------------------------
    void (*slider)(const Rect&, const SliderResult&, const ChromeTheme&,
                   const char*, float, float, float);
    void (*drag_float)(const Rect&, const DragFloatResult&, const ChromeTheme&,
                       const char*, float);

    // --- the busy rows ---------------------------------------------------
    void (*spinner)(const Rect&, const SpinnerResult&, const ChromeTheme&);
    void (*progress_indeterminate)(const Rect&, const IndeterminateResult&,
                                   const ChromeTheme&);

    // --- the strip and field rows ----------------------------------------
    void (*tab)(const Rect&, const TabResult&, const ChromeTheme&,
                const char*, bool);
    void (*combo)(const Rect&, const ComboResult&, const ChromeTheme&,
                  const char*, const char* const*, int, int, bool);
    void (*text_input)(const Rect&, const TextInputResult&, const ChromeTheme&,
                       const char*, const char*, int);

    // --- the chrome rows ---------------------------------------------------
    // Every surface and floating window a screen sits on.
    void (*panel)(const Rect&, const ChromeTheme&);
    void (*popup_frame)(const Rect&, const ChromeTheme&);
    void (*scrollbar)(const Rect&, const ScrollbarThumb&, const ChromeTheme&);

    // --- the menu rows -------------------------------------------------
    void (*menu_item)(const Rect&, const MenuItemPaint&, const ChromeTheme&,
                      const MenuItem&, const MenuGeometry&);
    void (*menu_item_separator)(const Rect&, const ChromeTheme&);
    void (*menu_title)(const Rect&, const MenuTitlePaint&, const ChromeTheme&,
                       const char*);

    // --- the data rows ---------------------------------------------------
    void (*table_header)(const TableGeometry&, bool, const ChromeTheme&);
    void (*table_row)(const Rect&, const TableRowPaint&, const char* const*,
                      const TableGeometry&, const ChromeTheme&);
    void (*toast)(const Rect&, ToastSeverity, const ChromeTheme&);
};

// The stock look: every row filled with the library's own draw - the
// exact functions the composed widgets call when the theme carries no
// table (they keep their plain signatures in widgets.hpp; a skin can
// call any of them to compose over the stock look).
inline constexpr WidgetPainters kStockPainters{
    &button_draw,
    &checkbox_draw,
    &radio_draw,
    &toggle_switch_draw,
    &collapsing_header_draw,
    &selectable_draw,
    &slider_draw,
    &drag_float_draw,
    &spinner_draw,
    &progress_indeterminate_draw,
    &tab_draw,
    &combo_draw,
    &text_input_draw,
    &panel_draw,
    &popup_frame_draw,
    &scrollbar_draw,
    &menu_item_draw,
    &menu_item_separator_draw,
    &menu_title_draw,
    &table_header_draw,
    &table_row_draw,
    &toast_draw,
};

}  // namespace ws::ui::gui
