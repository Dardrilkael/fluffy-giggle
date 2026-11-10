## ws::ui::gui widgets (src/gui/)
- widgets_detail.hpp: shared helpers + per-widget state structs (private)
- widgets_buttons.cpp: button, checkbox, radio, toggle_switch
- widgets_basic.cpp: collapsing_header, selectable, text, separator, progress,
  spinner, progress_indeterminate, status_bar (+ fit_text)
- widgets_sliders.cpp: slider_float/int, drag_float
- widgets_num_input.cpp: input_float/input_int - the typed numeric
  rows over text_input's machinery (the pure parse/format pair, the
  commit edges, the property grid's own mapping)
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
- text_input (+ EditBuffer/TextInputState/is_insertable public; the
  behavior's filter parameter is the seam a constrained field
  narrows its charset through)
- input_float (one draw and one state for both ladders; NumInputState,
  the numeric filters, input_parse_float/input_parse_int and
  input_format_float travel public with it) - widgets_num_input.cpp
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

## The skin pass's own halves (P5-P7, on top of the map above)

- `painters.hpp` also carries the ICON mapping: one extra row,
  `icon_of(theme, Icon) -> r2d::Sprite` (a LOOKUP, not a draw; the
  theme rides because a Sprite is font-bound), folded by
  `icon_sprite_of` - stock answers the theme font's own glyph out of
  the private-use set every bake carries (0xE000+ seventeen procedural
  shapes - the seven chrome markers plus the toolbar ten app actions
  cover, see src/text/font.h's icon half; `Font::find_glyph` is the
  carried-only resolution). Sentinel sprite = draw the pre-icon square.
- The r2d layered pair lives in the renderer, not here:
  `draw_rounded_outline` (exactly 2 quads - outer color + inner face
  inset by the stroke; src-over cannot punch a hole, so `face` is the
  caller's surface) and `draw_shadow` (the documented 4-layer stack).
  The stock adoption: panel_chrome/popup_frame_draw draw the border as
  the outline at the role radii + border_width, popups/toasts/menus
  shadow only when the theme's shadow tokens say so (the clip escapes
  open on `shadow_bounds`), ring_draw expands by `focus_ring_width`,
  the toggle track and scrollbar thumb read the pill role.
- `snap_v`/`snap_rect` (widgets_detail.hpp): the stock painters' static
  rects snap to whole device pixels; behavior rects and ANIMATED values
  never do.
- ButtonStyle { secondary, primary, ghost, danger } rides ButtonResult
  (the painter row's signature stays frozen); faces resolve through the
  tokens - the danger family derives in apply_accent and is checked
  like control_on (face 3:1 both backgrounds, the on-accent label on
  the face and its pressed rung, hover exempt).
