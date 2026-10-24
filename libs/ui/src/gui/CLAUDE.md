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
- theme_color.cpp / theme_data.cpp: the stage-10 theme data half - the
  OKLCH/WCAG color math + accent derivation, and the ThemeData document
  around ChromeTheme. Pure value types, not widgets; the widget-file rule
  below does not apply.
- theme_json.cpp: the stage-10 persistence half - read_theme/write_theme
  (the settings json/migrations seam) and ThemeFile (load, atomic save,
  the byte-compare hot-reload poller with its injected clock). Same
  value-type purity, no widgets.
Rule: one widget family per file; never add a widget to a file already over ~400 lines.
