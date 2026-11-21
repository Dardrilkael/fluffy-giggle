#pragma once

// apps/circsim_editor - the editor's one custom widget painter.
//
// The library's painter seam (ws/ui/gui/painters.hpp) lets an app replace
// any subset of widget draw rows without forking the library. The editor
// replaces exactly one: the button row, so the toolbar's primary action
// ("Simulate") wears the accent face and reads as THE call to action
// beside the quiet secondary buttons. Every other button, and every
// other widget, keeps the stock draw.
//
// Kept out of ui_style.hpp on purpose: that header is pure data the
// headless style suite includes; this one pulls the widget draws.

#include <ws/ui/gui/painters.hpp>

#include <cstddef>
#include <cstring>

namespace editor::style {

// A button label may carry an "##id" suffix; only the visible part counts.
[[nodiscard]] inline bool is_primary_label(const char* label) noexcept {
    if (label == nullptr) {
        return false;
    }
    constexpr const char kPrimary[] = "Simulate";
    constexpr std::size_t kLen = sizeof kPrimary - 1;
    const std::size_t visible = std::strcspn(label, "#");
    return visible == kLen && std::strncmp(label, kPrimary, kLen) == 0;
}

// The stock button, re-skinned through a theme copy for the primary
// action: accent face, accent_hover on hover, the pressed face as the
// theme's own, white-on-accent label. Disabled state, the focus ring and
// the icon all keep riding the stock draw.
inline void primary_button_draw(const ws::ui::gui::Rect& rect,
                                const ws::ui::gui::ButtonResult& result,
                                const ws::ui::gui::ChromeTheme& theme,
                                const char* label,
                                ws::ui::r2d::Sprite icon) {
    if (!is_primary_label(label)) {
        ws::ui::gui::button_draw(rect, result, theme, label, icon);
        return;
    }
    ws::ui::gui::ChromeTheme accent = theme;
    accent.control_bg = theme.control_on;
    accent.control_hovered = theme.accent_hover;
    accent.text = theme.text_on_accent;
    ws::ui::gui::button_draw(rect, result, accent, label, icon);
}

// The table the app's theme points at: the stock rows plus the one
// override. A function-local static, so its lifetime outlives every
// theme copy (the seam's lifetime rule).
[[nodiscard]] inline const ws::ui::gui::WidgetPainters& painters() {
    static const ws::ui::gui::WidgetPainters table = [] {
        ws::ui::gui::WidgetPainters t = ws::ui::gui::kStockPainters;
        t.button = &primary_button_draw;
        return t;
    }();
    return table;
}

}  // namespace editor::style
