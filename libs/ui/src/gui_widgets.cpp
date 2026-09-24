// ws::ui::gui widget functions (buildplan Phase 5+). One function per
// widget: lay out through LayoutCursor, hit-test through GuiContext,
// emit draws through r2d - identity, layout and drawing stay one seam.
// Every color and metric comes in through the ChromeTheme parameter;
// the functions themselves stay stateless.

#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <cstring>

namespace ws::ui::gui {
namespace {

// The label placeholder until r2d has text: a centered bar whose width
// tracks the label length, so sibling buttons stay visually distinct.
// Clamped to the face (theme padding each side) and never negative, so
// a narrow button cannot hand r2d a backwards bar.
float label_bar_width(const char* label, float max_width) {
    const std::size_t chars = label != nullptr ? std::strlen(label) : 0;
    const float wanted = static_cast<float>(chars) * 6.0f;
    return std::max(0.0f, std::min(wanted, max_width));
}

}  // namespace

bool button(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
            const char* label, float width) {
    width = std::max(width, 0.0f);  // a negative width would draw a mirrored quad

    // One seam: the id keys the hit-test AND any persistent state; the
    // rect is layout's answer for this frame; the draws land exactly
    // where hit-testing says the widget is.
    const WidgetId id = gui.ids().current(label);
    const Rect rect = layout.next(width, theme.control_height);
    const HitTest hit = gui.hit_test(id, rect);

    std::uint32_t face = theme.control_bg;
    if (hit.pressed) {
        face = theme.control_pressed;  // owned by the press, even if dragged away
    } else if (hit.hovered) {
        face = theme.control_hovered;
    }
    r2d::draw_quad(rect.x, rect.y, rect.width, rect.height, face);

    // The label bar: half the control height, centered both ways. A
    // zero-width bar (empty label or a degenerate button) draws nothing.
    const float bar_height = rect.height * 0.5f;
    const float bar_width = label_bar_width(label, width - 2.0f * theme.padding);
    if (bar_width > 0.0f) {
        r2d::draw_quad(rect.x + (rect.width - bar_width) * 0.5f,
                       rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                       bar_height, theme.text);
    }

    return hit.clicked;
}

}  // namespace ws::ui::gui
