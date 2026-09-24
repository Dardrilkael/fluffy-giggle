// ws::ui::gui widget functions (buildplan Phase 5+). One function per
// widget: lay out through LayoutCursor, hit-test through GuiContext, emit
// draws through r2d - identity, layout and drawing stay one seam. The
// colors here are Phase 5 placeholders; Phase 6 moves them into a
// ChromeTheme struct the caller passes in.

#include <ws/ui/gui/widgets.hpp>

#include <ws/ui/renderer2d.hpp>

#include <algorithm>
#include <cstring>

namespace ws::ui::gui {
namespace {

// Standard control height until the theme lands (Phase 6).
constexpr float kControlHeight = 24.0f;

// Phase 5 placeholder palette (Phase 6: ChromeTheme). r2d::rgba packs
// 0xRRGGBBAA.
constexpr std::uint32_t kButtonBase = r2d::rgba(70, 73, 78);
constexpr std::uint32_t kButtonHovered = r2d::rgba(92, 96, 102);
constexpr std::uint32_t kButtonPressed = r2d::rgba(72, 132, 192);
constexpr std::uint32_t kLabelColor = r2d::rgba(235, 238, 242);

// The label placeholder until r2d has text: a centered bar whose width
// tracks the label length, so sibling buttons stay visually distinct.
// Clamped to the face, so a long label never outgrows its button.
float label_bar_width(const char* label, float max_width) {
    const std::size_t chars = label != nullptr ? std::strlen(label) : 0;
    const float wanted = static_cast<float>(chars) * 6.0f;
    return std::min(wanted, max_width);
}

}  // namespace

bool button(GuiContext& gui, LayoutCursor& layout, const char* label,
            float width) {
    // One seam: the id keys the hit-test AND any persistent state; the
    // rect is layout's answer for this frame; the draws land exactly
    // where hit-testing says the widget is.
    const WidgetId id = gui.ids().current(label);
    const Rect rect = layout.next(width, kControlHeight);
    const HitTest hit = gui.hit_test(id, rect);

    std::uint32_t face = kButtonBase;
    if (hit.pressed) {
        face = kButtonPressed;  // owned by the press, even if dragged away
    } else if (hit.hovered) {
        face = kButtonHovered;
    }
    r2d::draw_quad(rect.x, rect.y, rect.width, rect.height, face);

    // The label bar: half the control height, centered both ways.
    constexpr float bar_height = kControlHeight * 0.5f;
    const float bar_width = label_bar_width(label, width - 8.0f);
    r2d::draw_quad(rect.x + (rect.width - bar_width) * 0.5f,
                   rect.y + (rect.height - bar_height) * 0.5f, bar_width,
                   bar_height, kLabelColor);

    return hit.clicked;
}

}  // namespace ws::ui::gui
