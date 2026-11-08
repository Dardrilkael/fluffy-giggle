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

namespace {

// The closable strip's tab hit shape: the tab's rect MINUS the round
// close button's circle. The tab and its x are two widgets over one
// rect, and the hit rules give a same-stratum press to whichever
// record order wins (a two-frame click: the later steal) or the first
// claimant (a same-frame tap: the click floor) - mutually exclusive
// answers. Cutting the circle out of the tab's OWN shape settles both
// timings the one way: the x is the only claimant inside the circle,
// the tab the only one outside it, and the x's square corners (inside
// the tab, outside the circle) fall through to the select.
struct TabCloseZone {
    Rect tab;
    Rect close;
};

bool tab_hit_outside_close(const void* user, float x, float y) {
    const auto* zone = static_cast<const TabCloseZone*>(user);
    if (!zone->tab.contains(x, y)) {
        return false;
    }
    const float dx = x - zone->close.center_x();
    const float dy = y - zone->close.center_y();
    const float r =
        std::min(zone->close.width, zone->close.height) * 0.5f;
    return dx * dx + dy * dy > r * r;
}

}  // namespace

bool tab_bar(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
             const char* label, const char* const* tabs, int count,
             int& active, float width, bool disabled, int* closed) {
    width = std::max(width, 0.0f);  // like button: no mirrored quads

    bool changed = false;
    if (closed != nullptr) {
        *closed = -1;  // no tab's x has fired (yet) this frame
    }
    // The strip's own scope: each tab ids by index under it, so duplicate
    // tab labels in one strip stay distinct widgets (the loop-children
    // discipline, without stringifying).
    gui.ids().push(label);
    for (int i = 0; i < count; ++i) {
        gui.ids().push(i);
        const Rect rect = layout.next(width, theme.control_height);
        // The x's geometry, BEFORE the tab's behavior: the tab's hit
        // shape needs the circle, and the label's fit budget needs the
        // zone. A small round 12-px hitbox at the tab's right edge,
        // padded in by the theme's own inset so it breathes inside the
        // tab instead of touching its border - small on purpose: the
        // tab is the fat target, the x is the precise one.
        const bool closable = closed != nullptr;
        constexpr float kCloseHit = 12.0f;
        const Rect close_rect{
            rect.x + rect.width - theme.padding - kCloseHit,
            rect.y + (rect.height - kCloseHit) * 0.5f, kCloseHit,
            kCloseHit};
        TabCloseZone zone{rect, close_rect};
        HitShape tab_shape;  // the plain rect when not closable
        if (closable) {
            tab_shape.kind = HitShape::Kind::predicate;
            tab_shape.predicate = &tab_hit_outside_close;
            tab_shape.user = &zone;
        }
        // Full citizenship, like a button, per tab - the behavior's fold
        // (registration in strip order so Tab walks the strip left to
        // right, state()'s one hit-test, the Enter/Space edge). The
        // trailing `disabled` parameter is the same lock on the whole
        // strip: registration skipped (Tab walks past), state() handed
        // the lock, and the selection below cannot move (a dead row
        // reports no click, holds no focus for the keys).
        const WidgetId tab_id = gui.ids().id("tab");
        TabResult result = tab_behavior(gui, tab_id, rect, disabled,
                                        tab_shape);
        fold_hover_t(gui, tab_id, result, theme);  // P4: the face traveler
        // P4: the accent traveler - the selected tab's underline fades
        // in under the tab's own id.
        result.accent_t = gui.animate_toward(tab_id, i == active ? 1.0f : 0.0f,
                                             motion_duration(theme));
        // The closable tab's label, pre-fitted around the x's zone: the
        // painter row fits to the tab minus padding as always, so the
        // reservation must already be spent here - the ellipsized text
        // hands the skin the same shape every tab label arrives in,
        // nothing about the row's signature changes.
        const char* tab_label = tabs != nullptr ? tabs[i] : nullptr;
        char label_scratch[128];
        if (closable && tab_label != nullptr) {
            const float budget = std::max(
                0.0f, rect.width - 2.0f * theme.padding -
                          (theme.padding + kCloseHit));
            const LabelFit fit = fit_text(tab_label, budget, theme,
                                          label_scratch, sizeof(label_scratch));
            tab_label = fit.text;
        }
        paint(theme, &WidgetPainters::tab, &tab_draw, rect, result, theme,
              tab_label, i == active);
        // A full click or the focused tab's Enter/Space moves the
        // selection; selecting the already-selected tab is a no-op that
        // reports nothing.
        if (result.clicked && i != active) {
            active = i;
            changed = true;
        }
        // The tab's own x: a ghost button in the circle - no resting
        // face (the tab's face shows through), a round hover/press disc,
        // and the muted x mark that goes full ink the moment the pointer
        // arrives. Hit-tested as the circle it is drawn as, registered
        // AFTER the tab so painter order and claim order agree.
        if (closable) {
            const WidgetId close_id = gui.ids().id("close");
            HitShape close_hit;
            close_hit.kind = HitShape::Kind::circle;
            ButtonResult close = button_behavior(gui, close_id, close_rect,
                                                 disabled, close_hit,
                                                 ButtonStyle::ghost);
            fold_hover_t(gui, close_id, close, theme);
            if (close.state == WidgetState::Pressed) {
                r2d::draw_rounded_quad(close_rect.x, close_rect.y,
                                       kCloseHit, kCloseHit,
                                       kCloseHit * 0.5f,
                                       theme.control_pressed);
            } else if (close.state == WidgetState::Hovered) {
                const float t =
                    close.hover_t >= 0.0f ? close.hover_t : 1.0f;
                r2d::draw_rounded_quad(
                    close_rect.x, close_rect.y, kCloseHit, kCloseHit,
                    kCloseHit * 0.5f,
                    fade_alpha(theme.control_hovered, t));
            }
            // The mark: the multiplication x every close button wears,
            // muted at rest so the label owns the tab and full ink under
            // the pointer. The fontless v1 spelling rides the bar, like
            // every label without a face.
            char glyph[16];
            const LabelFit mark = fit_text("\xC3\x97", -1.0f, theme, glyph,
                                           sizeof(glyph));
            if (mark.advance > 0.0f) {
                const bool live = close.state == WidgetState::Hovered ||
                                  close.state == WidgetState::Pressed;
                const std::uint32_t ink =
                    live ? theme.text : theme.text_muted;
                const float mx =
                    close_rect.x + (kCloseHit - mark.advance) * 0.5f;
                if (theme.font != nullptr) {
                    r2d::draw_text(mx, close_rect.y +
                                           (kCloseHit -
                                            theme.font->line_height()) *
                                               0.5f,
                                   mark.text, *theme.font, ink);
                } else {
                    r2d::draw_quad(mx, close_rect.y + kCloseHit * 0.5f - 2.0f,
                                   mark.advance, 4.0f, ink);
                }
            }
            // The keyboard's legibility: the ring, round like the button
            // it frames (Enter or Space on the focused x closes the tab,
            // exactly like a click).
            ring_draw(close.focused, close_rect, theme, kCloseHit * 0.5f);
            tooltip_text(gui, layout, theme, "close", close_rect,
                         "Close tab");
            if (close.activated()) {
                *closed = i;
            }
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

    // Face priority: the pointer's live states first - press, then
    // hover, EVEN on the selected tab (a one-tab strip's only tab is
    // always selected; a strip that never answers the pointer reads
    // dead) - then the selection's resting face. The selected tab
    // wears the PANEL's face plus the accent underline along its
    // bottom edge - the editor-tab look: it merges with the body it
    // owns instead of floating on the strip. Under the pointer it
    // takes the hover face like any control and KEEPS the underline,
    // so the selection stays legible while the tab answers. An idle
    // unselected tab wears NOTHING: text on the strip, like a menu
    // title at rest. A dead tab claims nothing, so its pressed/hovered
    // branches never fire - and a dead SELECTED tab falls to the
    // selection branch, its dimmed face and underline kept.
    if (result.state == WidgetState::Pressed) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               theme.control_pressed);
    } else if (result.state == WidgetState::Hovered && result.hover_t >= 0.0f) {
        // P4: an idle tab wears nothing, so the hover face fades IN
        // (alpha) rather than blending from another face.
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               fade_alpha(theme.control_hovered,
                                          result.hover_t));
    } else if (result.state == WidgetState::Hovered) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               theme.control_hovered);
    } else if (selected) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius,
                               dim(theme.panel_bg, dim_by));
    }
    // The underline: 2 px of accent flush with the bottom edge - the
    // one mark the selection needs once the face stopped shouting.
    // dim_by keeps a dead strip's selection faded. Drawn on every face
    // the selected tab wears, hover and press included: the selection
    // never blinks out from under the pointer.
    if (selected) {
        // P4: the underline fades in on the accent traveler (full
        // strength when no animation speaks).
        const float at =
            result.accent_t >= 0.0f ? result.accent_t : 1.0f;
        r2d::draw_quad(rect.x, rect.y + rect.height - 2.0f, rect.width,
                       2.0f, fade_alpha(dim(theme.control_on, dim_by), at));
    }

    // The tab's label, centered like a button's: fitted to the tab
    // minus its padding. An unselected tab's label wears the MUTED rung
    // - its own gray (#858585 on the dark stock), not an alpha dim of
    // body text - so the selection reads at a glance, not glyph by
    // glyph, and the inactive strip never competes with the panel it
    // fronts. Tab labels never id (the strip ids by
    // INDEX), so a "##" in a tab label is pure display sugar: it
    // cuts and keys nothing.
    char scratch[128];
    const LabelFit fit =
        fit_text(label,
                 std::max(0.0f, rect.width - 2.0f * theme.padding), theme,
                 scratch, sizeof(scratch));
    const std::uint32_t label_color =
        selected ? theme.text : theme.text_muted;
    draw_label(rect.x + (rect.width - fit.advance) * 0.5f, rect.y,
               rect.height, fit, dim(label_color, dim_by), theme);
}

Rect begin_panel(GuiContext& gui, LayoutCursor& layout,
                 const ChromeTheme& theme, const char* label,
                 const Rect& bounds) {
    // Chrome first, BEFORE the clip push, so the frame and face never clip
    // against the content rect they contain (see panel_chrome) - through
    // the painter seam, so a skin's panel row restyles every surface.
    paint(theme, &WidgetPainters::panel, &panel_draw, bounds, theme);

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
    // paints before it pushes anything - the same draw both call. The
    // stock function itself never routes (a skin's panel row replaces
    // it whole); the composed callers route TO it.
    panel_chrome(bounds, theme);
}

}  // namespace ws::ui::gui
