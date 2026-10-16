// ws::ui::gui widget functions, one family per file (src/gui/CLAUDE.md):
// the static chrome rows - collapsing_header, selectable, text,
// separator, progress - plus fit_text, the public label measure every
// family file fits its labels through. One definition for the archive:
// it lives here, beside the text widget that shows what it measures.

#include "widgets_detail.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace ws::ui::gui {

// fit_text's body. The display cut first (label_display - "##" id
// suffixes never draw), then the width pass: the whole display when it
// fits, and only a miss truncates - longest fitting prefix + ASCII
// "..." - the cut backed up over UTF-8 continuation bytes so no
// codepoint splits (the fontless theme never draws the text, but the
// answer is a C string any caller could). Both measures ride one
// lambda: the theme font's honest glyph advance, or the per-byte
// estimate the v1 bars size from. The copy lands in the CALLER'S
// scratch - the only buffer fit_text ever writes - and only when
// something changed, so the mid-frame no-allocation rule holds on
// every path.
LabelFit fit_text(const char* label, float max_width,
                  const ChromeTheme& theme, char* scratch,
                  std::size_t scratch_bytes) {
    const std::string_view display = label_display(label);
    if (display.empty()) {
        return {"", 0.0f};  // nothing to say: nothing to draw
    }

    // A marker cut the display when the label continues past it: the
    // view over the label is not null-terminated at the cut, so the
    // display moves into scratch as draw-ready bytes. No scratch, no
    // cut - and nothing drawable.
    const char* text = label;
    if (label[display.size()] != '\0') {
        if (scratch == nullptr || scratch_bytes < display.size() + 1) {
            return {"", 0.0f};
        }
        std::memcpy(scratch, display.data(), display.size());
        scratch[display.size()] = '\0';
        text = scratch;
    }

    // The advance of the display as it stands: the font's measure, or
    // (fontless) the estimate - per byte, the v1 bar's arithmetic.
    const auto measure = [&](const char* s, std::size_t bytes) {
        if (theme.font != nullptr) {
            return theme.font->measure_text(s);
        }
        return static_cast<float>(bytes) * theme.label_char_width;
    };
    const float whole = measure(text, display.size());
    if (max_width < 0.0f || whole <= max_width) {
        return {text, whole};  // fits (or nobody asked for a width)
    }

    // Truncation: prefixes longest-first, each composed in scratch as
    // prefix + "..." and measured whole. The whole display already
    // missed, so the longest candidate cuts at least one byte; k = 0
    // is the bare "..." - the last resort, and past it nothing draws.
    if (scratch == nullptr || scratch_bytes < 4) {
        return {"", 0.0f};  // not even the bare "..." could be built
    }
    const std::size_t cap = scratch_bytes - 1;  // room for the terminator
    std::size_t k_max = display.size() - 1;  // cut at least one byte
    if (k_max > cap - 3) {
        k_max = cap - 3;  // the longest prefix that + "..." still fits
    }
    for (std::size_t k = k_max + 1; k-- > 0;) {
        // UTF-8: back up while the byte at the cut continues the
        // codepoint before it - the prefix keeps whole codepoints
        // (corrupt bytes stay corrupt; utf8_next forgives them).
        while (k > 0 && k < display.size() &&
               (static_cast<unsigned char>(display[k]) & 0xC0u) == 0x80u) {
            --k;
        }
        std::memcpy(scratch, display.data(), k);
        scratch[k] = '.';
        scratch[k + 1] = '.';
        scratch[k + 2] = '.';
        scratch[k + 3] = '\0';
        const float advance = measure(scratch, k + 3);
        if (advance > 0.0f && advance <= max_width) {
            return {scratch, advance};
        }
        if (k == 0) {
            break;  // the bare "..." was the last candidate
        }
    }
    return {"", 0.0f};  // not even "..." fits: nothing drawable
}

bool collapsing_header(GuiContext& gui, LayoutCursor& layout,
                       const ChromeTheme& theme, const char* label,
                       bool& open, float width) {
    // Fill-width: a header wants its container's line.
    const Row row = begin_row(gui, layout, theme, label, width);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard toggle: Enter or Space flips the focused header,
    // edge-only - a click with no pointer involved.
    const bool key_toggled = key_activated(gui, id);

    if (hit.clicked || key_toggled) {
        open = !open;  // the caller's bool is the state; a click flips it
    }

    // The focus ring: the on-accent as a 1px outline behind the row, so
    // Tab navigation is legible. Skipped for degenerate rows.
    focus_ring(gui, id, rect, theme);

    // The disabled look: the dead face for the row, the marker and the
    // label dimmed through the one helper.
    const float dim_by = dim_factor(result.state, theme);

    // The row: the faces a button wears, rounded by the theme.
    const std::uint32_t face = control_face(result.state, theme);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, face);

    // The marker: a square inset like a checkbox's fill - the faint
    // border color closed, the accent open. The state at a glance. The
    // inset clears the row's rounded corners the same way the checkbox
    // fill clears its box's (a heavily-rounded theme cannot let the
    // square poke past the arcs).
    const float box = theme.control_height;
    const float inset = std::max(theme.check_inset, theme.corner_radius);
    const float marker = box - 2.0f * inset;
    if (marker > 0.0f) {
        r2d::draw_quad(rect.x + inset, rect.y + inset, marker, marker,
                       open ? dim(theme.control_on, dim_by) : theme.border);
    }

    // The label beside the marker, like a checkbox's - LEFT-aligned: a
    // header reads from the left. Fitted to the room the marker leaves
    // (minus a face inset each side).
    const float gap = theme.spacing;
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, rect.width - box - gap -
                                          2.0f * theme.label_inset),
                 theme, scratch, sizeof(scratch));
    draw_label(rect.x + box + gap, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
    // The answer is the state AFTER the flip: the caller's `if` places
    // this frame's children when the click just opened it.
    return open;
}

bool selectable(GuiContext& gui, LayoutCursor& layout, const ChromeTheme& theme,
               const char* label, bool selected, float width) {
    // Fill-width: list rows want their container's line.
    const Row row = begin_row(gui, layout, theme, label, width);
    const WidgetId id = row.id;
    const Rect& rect = row.rect;
    const WidgetResult& result = row.result;
    const HitTest& hit = result.hit;

    // Keyboard select: Enter or Space reports the focused row, edge-only
    // - a click with no pointer involved. The selection itself stays
    // the caller's to write.
    const bool key_clicked = key_activated(gui, id);

    // The focus ring: the on-accent as a 1px outline behind the row.
    // Skipped for degenerate rows.
    focus_ring(gui, id, rect, theme);

    const float dim_by = dim_factor(result.state, theme);

    // The bare-list face: an unselected, unhovered row draws nothing at
    // all - hover, press and the disabled scope take the usual faces.
    if (const auto face = list_face(result.state, theme)) {
        r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                               theme.corner_radius, *face);
    }

    // The selection marker: a 3-px accent bar down the row's left edge -
    // the v1 look, a hardcoded pixel like the separator's 2. It rides
    // every face (or the bare row), dimmed under the disabled scope.
    if (selected && rect.width > 0.0f && rect.height > 0.0f) {
        r2d::draw_quad(rect.x, rect.y, 3.0f, rect.height,
                       dim(theme.control_on, dim_by));
    }

    // The label, left-aligned past the bar's room (selected and
    // unselected rows align).
    char scratch[128];
    const LabelFit fit =
        fit_text(label, std::max(0.0f, rect.width - theme.check_inset -
                                          theme.label_inset),
                 theme, scratch, sizeof(scratch));
    draw_label(rect.x + theme.check_inset, rect.y, rect.height, fit,
               dim(theme.text, dim_by), theme);
    return hit.clicked || key_clicked;
}

void text(LayoutCursor& layout, const ChromeTheme& theme,
          const char* label, float width) {
    // Chrome: no id, no hit-test, no state - it lays out and draws, and
    // that is all. A caller wanting a faded label dims its own theme
    // copy; the gui does not invent a Disabled state for something that
    // cannot be disabled.
    char scratch[128];
    const LabelFit fit = fit_text(label, width < 0.0f ? -1.0f : width, theme,
                                  scratch, sizeof(scratch));
    // A negative width sizes the row to the label's DISPLAY advance -
    // marker-cut, never truncated, like a checkbox's sizing fit (and a
    // "##" suffix neither ids nor sizes anything here: it just never
    // draws). A fixed width pins the row and truncates the label into
    // it instead. fit_text's own tests own the truncation ladder.
    const Rect rect =
        layout.next(width < 0.0f ? fit.advance : width, theme.control_height);
    draw_label(rect.x, rect.y, rect.height, fit, theme.text, theme);
}

void separator(LayoutCursor& layout, const ChromeTheme& theme, float width) {
    // Fill-width, like button: a negative width means "the rest of the
    // line", resolved against the cursor before the clamp.
    resolve_width(width, layout);
    // A 2-px rule - a hardcoded pixel like the focus ring's 1, not a
    // theme metric. theme.border is the faint line that never competes
    // with content, and the flow's own spacing gives it air on both
    // sides.
    const Rect rect = layout.next(width, 2.0f);
    r2d::draw_quad(rect.x, rect.y, rect.width, rect.height, theme.border);
}

void progress(LayoutCursor& layout, const ChromeTheme& theme,
              float fraction, float width) {
    // Fill-width, like button: a negative width means "the rest of the
    // line", resolved against the cursor before the clamp.
    resolve_width(width, layout);
    // Clamp to [0, 1], NaN-safe: the inverted comparisons catch a NaN
    // (it fails >= 0) and read it as empty rather than letting it
    // through to the draw.
    if (!(fraction >= 0.0f)) {
        fraction = 0.0f;
    }
    if (!(fraction <= 1.0f)) {
        fraction = 1.0f;
    }
    const Rect rect = layout.next(width, theme.control_height);
    r2d::draw_rounded_quad(rect.x, rect.y, rect.width, rect.height,
                           theme.corner_radius, theme.control_bg);
    // The fill: the accent over the track, rounded with the theme; the
    // draw clamps the radius to the fill's own half-height, so a thin
    // sliver reads as a dot rather than an over-rounded bar.
    const float fill = fraction * rect.width;
    if (fill > 0.0f) {
        r2d::draw_rounded_quad(rect.x, rect.y, fill, rect.height,
                               theme.corner_radius, theme.control_on);
    }
}

}  // namespace ws::ui::gui
