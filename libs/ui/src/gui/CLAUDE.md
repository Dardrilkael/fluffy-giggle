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
- widgets_combo.cpp / widgets_text_input.cpp / widgets_table.cpp
Rule: one widget family per file; never add a widget to a file already over ~400 lines.
