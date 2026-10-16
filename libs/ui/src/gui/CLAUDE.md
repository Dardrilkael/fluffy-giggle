## ws::ui::gui widgets (src/ui/gui/)
- widgets_detail.hpp: shared helpers + per-widget state structs (private)
- widgets_buttons.cpp: button, checkbox, radio, toggle_switch
- widgets_basic.cpp: collapsing_header, selectable, text, separator, progress
- widgets_sliders.cpp: slider_float/int, drag_float
- widgets_panels.cpp: tab_bar, panel
- widgets_scroll.cpp: scroll panel (+ drag/thumb state)
- widgets_overlays.cpp: popup, tooltip, modal
- widgets_combo.cpp / widgets_text_input.cpp / widgets_table.cpp
Rule: one widget family per file; never add a widget to a file already over ~400 lines.