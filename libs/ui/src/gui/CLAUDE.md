## ws::ui::gui widgets (src/gui/)
- widgets_detail.hpp: shared helpers + per-widget state structs (private)
- widgets_buttons.cpp: button, checkbox, radio, toggle_switch
- widgets_basic.cpp: collapsing_header, selectable, text, separator, progress,
  spinner, progress_indeterminate, status_bar (+ fit_text)
- widgets_sliders.cpp: slider_float/int, drag_float
- widgets_panels.cpp: tab_bar, panel
- widgets_scroll.cpp: scroll panel (+ drag/thumb state)
- widgets_overlays.cpp: popup, tooltip, modal
- widgets_dialogs.cpp: modal_dialog (the stock button sets over begin_modal)
- widgets_menus.cpp: menu_bar, context_menu (+ the shared item renderer/chain)
- widgets_toasts.cpp: the Toasts store + draw_toasts (the toast stratum)
- widgets_jobs.cpp: the ws::jobs bridge's out-of-line half - the JobProgress
  mirror's store and the job_view state-to-view mapping (the row and toast
  templates stay in job_progress.hpp with their JobHandle<T> parameter)
- widgets_combo.cpp / widgets_text_input.cpp / widgets_table.cpp
- command_views.cpp / command_palette.cpp / command_keymap.cpp: the
  ws::app bridge (stage 13 phase 4) - the shared hint + menu builder +
  toolbar; the palette's fuzzy scorer/ranker + the modal over them; and
  the keymap editor with its Record capture. One widget family, one
  file; the bridge's shared header is include/ws/ui/gui/command_views.hpp
- theme_color.cpp / theme_data.cpp: the stage-10 theme data half - the
  OKLCH/WCAG color math + accent derivation, and the ThemeData document
  around ChromeTheme. Pure value types, not widgets; the widget-file rule
  below does not apply.
- theme_json.cpp: the stage-10 persistence half - read_theme/write_theme
  (the settings json/migrations seam) and ThemeFile (load, atomic save,
  the byte-compare hot-reload poller with its injected clock). Same
  value-type purity, no widgets.
Rule: one widget family per file; never add a widget to a file already over ~400 lines.

## The behavior/draw split - complete map (the ui-skin plan's P0 audit)

Every stateful family is two public halves: `<widget>_behavior` (the
interaction and citizenship ladder, answering a paint-ready result that
folds WidgetPaint; draws nothing) and `<widget>_draw` (the stock look;
no GuiContext, recomputing its own label fits). The composed widget is
their thin fold; a custom look drives the behavior and paints its own
pixels over the answer. The pairs:

- button, checkbox, radio, toggle_switch (widgets_buttons.cpp)
- collapsing_header, selectable (widgets_basic.cpp)
- slider (shared by slider_float/slider_int), drag_float
  (widgets_sliders.cpp)
- spinner, progress_indeterminate (widgets_basic.cpp)
- tab (widgets_panels.cpp - the strip loops the per-tab pair)
- combo (the ROW pair; the dropdown stays the composed widget's own
  half - a custom combo lays its list through place_popup/begin_popup)
- text_input (+ EditBuffer/TextInputState/is_insertable public)
- the scroll halves: scroll_drag_behavior / scroll_thumb_behavior /
  scrollbar_draw, with ScrollState/ScrollDragState public
- panel_draw, popup_frame_draw (every floating window's chrome - the
  overlay scopes and toast_draw call it at their bounds),
  popup_placement / place_popup
- menu_item_draw / menu_item_separator_draw / menu_title_draw (the
  chain and bar own the interaction - MenuState stays private; the
  rows' looks ride MenuGeometry + MenuItemPaint/MenuTitlePaint)
- table_header_draw / table_row_draw (the composed table owns the
  geometry resolution and the grid walk; the looks ride TableGeometry
  + TableRowPaint)
- toast_draw (the chrome + severity band; the scope and rows stay the
  composed draw_toasts' half)
- the statics (text, separator, progress, status_bar) are draw-only
  and unpaired - chrome, not citizens.

The painters table (the P3 seam, WidgetPainters in
include/ws/ui/gui/painters.hpp) routes the composed widgets through
exactly these draw signatures, one fn-pointer row each; nullptr =
stock. A painter receives the behavior's answer (state/focus/hit),
the rect, the theme and the row's data; it draws ONLY - no context,
no state change, no allocation - and may compose (a skin's row may
call any stock draw).
