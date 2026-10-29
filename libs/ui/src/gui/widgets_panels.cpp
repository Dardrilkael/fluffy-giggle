// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the container rows - the tab strip as a pair (tab_behavior/tab_draw,
// one tab's interaction and pixels; tab_bar owns the strip: the id scope
// and the pitch), the begin/end_panel scope every widget lives inside
// (chrome, the clip pair, the id scope and the wheel claim), and
// panel_draw: that chrome alone for a custom container.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>

namespace ws::ui::gui {

bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width, bool disabled) {
    width = std::max(width, 0.0f);  // like button: no mirrored quads

    bool changed = false;
    // The strip's own scope: each tab ids by index under it, so duplicate
    // tab labels in one strip stay distinct widgets (the loop-children
    // discipline, without stringifying).
    gui.ids().push(label);
    for (int i = 0; i < count; ++i) {
        gui.ids().push(i);
        const Rect rect = layout.next(width, theme.control_height);
        // Full citizenship, like a button, per tab - the behavior's fold
        // (registration in strip order so Tab walks the strip left to
        // right, state()'s one hit-test, the Enter/Space edge). The
        // trailing `disabled` parameter is the same lock on the whole
        // strip: registration skipped (Tab walks past), state() handed
        // the lock, and the selection below cannot move (a dead row
        // reports no click, holds no focus for the keys).
        const WidgetId tab_id = gui.ids().id("tab");
        const TabResult result = tab_behavior(gui, tab_id, rect, disabled);
        tab_draw(rect, result, theme, tabs != nullptr ? tabs[i] : nullptr,
                 i == active);
        // A full click or the focused tab's Enter/Space moves the
        // selection; selecting the already-selected tab is a no-op that
        // reports nothing.
        if (result.clicked && i != active) {
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

TabResult tab_behavior(GuiContext& gui, WidgetId id, const Rect& rect,
                       bool disabled, const HitShape& hit_shape) {
    TabResult result{begin_widget_paint(gui, id, rect, disabled, hit_shape)};
    // Enter or Space on the focused tab selects it - folded with the
    // click into the one "pick me" signal a tab has.
    result.clicked = result.hit.clicked || key_activated(gui, id);
    return result;
}

void tab_draw(const Rect& rect, const TabResult& result,
              const ChromeTheme& theme, const char* label, bool selected) {
    // The focus ring: the on-accent as a 1px outline behind the tab, so
    // Tab navigation is legible. Skipped for degenerate rows.
    ring_draw(result.focused, rect, theme);

    // The disabled look: the dead face for an idle tab, the selected
    // tab's accent and the label dimmed - the selection stays
    // legible in a dead strip, just faded.
    const float dim_by = dim_factor(result.state, theme);

    // Face priority: press feedback even on the selected tab, then
    // the selection itself, then hover. The selected tab wears the
    // PANEL's face plus the accent underline along its bottom edge -
    // the editor-tab look: it merges with the body it owns instead of
    // floating on the strip. An idle unselected tab wears NOTHING:
    // text on the strip, like a menu title at rest. A dead tab claims
    // nothing, so its pressed/hover branches never fire.
    if (result.state == WidgetState::Pressed) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               theme.control_pressed);
    } else if (selected) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               dim(theme.panel_bg, dim_by));
        // The underline: 2 px of accent flush with the bottom edge -
        // the one mark the selection needs once the face stopped
        // shouting. dim_by keeps a dead strip's selection faded.
        r2d::draw_quad(rect.x, rect.y + rect.height - 2.0f, rect.width,
                       2.0f, dim(theme.control_on, dim_by));
    } else if (result.state == WidgetState::Hovered) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               theme.control_hovered);
    } else if (result.state == WidgetState::Disabled) {
        // no face: a dead tab is dimmed text on the strip
    }

    // The tab's label, centered like a button's: fitted to the tab
    // minus its padding. An unselected tab's label sits a step quieter
    // than the selected one's - the selection reads at a glance, not
    // glyph by glyph. Tab labels never id (the strip ids by
    // INDEX), so a "##" in a tab label is pure display sugar: it
    // cuts and keys nothing.
    char scratch[128];
    const LabelFit fit =
        fit_text(label,
                 std::max(0.0f, rect.width - 2.0f * theme.padding), theme,
                 scratch, sizeof(scratch));
    const std::uint32_t label_color =
        selected ? theme.text : dim(theme.text, 0.72f);
    draw_label(rect.x + (rect.width - fit.advance) * 0.5f, rect.y,
               rect.height, fit, dim(label_color, dim_by), theme);
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

void panel_draw(const Rect& bounds, const ChromeTheme& theme) {
    // The one-line public spelling of the private chrome begin_panel
    // paints before it pushes anything - the same draw both call.
    panel_chrome(bounds, theme);
}

}  // namespace ws::ui::gui
