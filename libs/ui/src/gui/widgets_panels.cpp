// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the container rows - the tab_bar strip and the begin/end_panel
// scope every widget lives inside: chrome, the clip pair, the id
// scope and the wheel claim.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width) {
    width = std::max(width, 0.0f);  // like button: no mirrored quads

    bool changed = false;
    // The strip's own scope: each tab ids by index under it, so duplicate
    // tab labels in one strip stay distinct widgets (the loop-children
    // discipline, without stringifying).
    gui.ids().push(label);
    for (int i = 0; i < count; ++i) {
        gui.ids().push(i);
        const Rect rect = layout.next(width, theme.control_height);
        // Full citizenship, like a button, per tab: each tab registers
        // in strip order (Tab walks the strip left to right), state()
        // runs the one hit-test and derives the face, and Enter or
        // Space selects the focused tab.
        const WidgetId tab_id = gui.ids().id("tab");
        gui.register_focusable(tab_id);
        const WidgetResult result = gui.state(tab_id, rect);
        const HitTest& hit = result.hit;
        const bool key_selected = key_activated(gui, tab_id);

        focus_ring(gui, tab_id, rect, theme);

        // The disabled look: the dead face for an idle tab, the selected
        // tab's accent and the label dimmed - the selection stays
        // legible in a dead strip, just faded.
        const float dim_by = dim_factor(result.state, theme);

        // Face priority: press feedback even on the selected tab, then
        // the selection accent, then hover, then the dead face, then
        // idle. A dead tab claims nothing, so its pressed/hover branches
        // never fire - the Disabled slot is for the idle one.
        std::uint32_t face = theme.control_bg;
        if (result.state == WidgetState::Pressed) {
            face = theme.control_pressed;
        } else if (i == active) {
            face = dim(theme.control_on, dim_by);
        } else if (result.state == WidgetState::Hovered) {
            face = theme.control_hovered;
        } else if (result.state == WidgetState::Disabled) {
            face = theme.control_disabled;
        }
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, face);

        // The tab's label, centered like a button's: fitted to the tab
        // minus its padding. Tab labels never id (the strip ids by
        // INDEX), so a "##" in a tab label is pure display sugar: it
        // cuts and keys nothing.
        const char* tab_label = tabs != nullptr ? tabs[i] : nullptr;
        char scratch[128];
        const LabelFit fit =
            fit_text(tab_label,
                     std::max(0.0f, rect.width - 2.0f * theme.padding), theme,
                     scratch, sizeof(scratch));
        draw_label(rect.x + (rect.width - fit.advance) * 0.5f, rect.y,
                   rect.height, fit, dim(theme.text, dim_by), theme);

        // A full click or the focused tab's Enter/Space moves the
        // selection; selecting the already-selected tab is a no-op that
        // reports nothing.
        if ((hit.clicked || key_selected) && i != active) {
            active = i;
            changed = true;
        }
        gui.ids().pop();
        if (i + 1 < count) {
            // One line, but FLUSH: the strip is one widget's row, not
            // flow items, so the tabs sit at fixed pitch (the strip is
            // count*width wide, the documented geometry) - the explicit
            // 0 gap overrides the panel's item spacing, which would
            // otherwise gap them.
            layout.same_line(0.0f);
        }
    }
    gui.ids().pop();
    return changed;
}

Rect begin_panel(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds) {
    // Chrome first, BEFORE the clip push, so the frame and face never clip
    // against the content rect they contain (see panel_chrome).
    panel_chrome(bounds, theme);

    // Then the four pushes: the layout panel (it computes the content
    // rect), the clip at that rect and its hit-testing twin (children
    // cannot draw OR claim outside - what the clip hides cannot win the
    // pointer either), and the id scope (children id under the panel). A
    // degenerate content rect - an overflowed panel or bounds smaller
    // than the padding - pushes an empty clip: honestly nothing, rather
    // than children leaking outside.
    const Rect content = layout.begin_panel(bounds, theme.padding,
                                            theme.spacing);
    // A panel is a wheel claimant too - ALL panels are, scrollable or
    // not: the popup that covers a scroll panel eats the wheel even
    // though it scrolls nothing. Keyless: it claims, never consumes.
    gui.register_wheel_claim(bounds, nullptr);
    r2d::push_clip(content.x, content.y, content.width, content.height);
    gui.push_hit_clip(content);
    gui.ids().push(label);
    return content;
}

void end_panel(GuiContext& gui, LayoutCursor& layout) {
    // Exactly the reverse of begin_panel; each pop is a no-op against a
    // push that dropped (headless) or overflowed (the balanced clamps).
    gui.ids().pop();
    r2d::pop_clip();
    gui.pop_hit_clip();
    layout.end_panel();
}

}  // namespace ws::ui::gui
