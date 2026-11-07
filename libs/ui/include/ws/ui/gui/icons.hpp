#pragma once

// The built-in icon set (the ui-skin plan's P6): seven minimal interface
// glyphs baked into every font's own pages under a private-use codepoint
// range, exposed as one enum + a sprite mapping + a draw. The raster half
// is src/text/font.{h,cpp} (private to ui); this header is the public
// seam: widgets resolve their markers through it, and a skin re-points
// the mapping at its own atlas by overriding the painters' icon_of row.

#include <ws/ui/gui/theme.hpp>

#include <cmath>

namespace ws::ui::gui {

// The set, in its bake order (the index IS the codepoint offset - see
// font::kIconFirst). Seventeen glyphs, a floor not a ceiling: an app
// with real icon needs brings an atlas and overrides icon_of. The first
// seven are the chrome's own markers; the toolbar ten after them are
// the standard action glyphs app toolbars map their commands to (the
// circsim editor's New/Open/Save/Undo/... row).
enum class Icon : std::uint8_t {
    chevron_down,   // combo closed-row marker, collapsing header open
    chevron_right,  // collapsing header closed, the submenu arrow
    check,          // the checkbox's checked mark
    close,          // dismissal affordances
    search,
    plus,
    minus,
    // --- the toolbar ten (app action glyphs) ---
    document,         // New - a page with a folded corner
    folder,           // Open - the folder with its tab
    save,             // Save - the floppy
    undo,             // the arc + left head
    redo,             // undo's mirror
    trash,            // Delete - the bin
    rotate,           // Rotate - the clockwise circle arrow
    mirror,           // Mirror - the two meeting arrows
    wire,             // Place Wire - the ortho route between two dots
    clipboard_check,  // the check list (ERC and friends)
};

// The private-use codepoint the stock mapping serves for `icon`
// (0xE000 + the enum's index; the raster half's own constant).
[[nodiscard]] constexpr std::uint32_t icon_codepoint(Icon icon) {
    return 0xE000u + static_cast<std::uint32_t>(icon);
}

// The stock mapping: the icon's glyph sprite out of the theme's own font
// (every load_font/load_default_font bake carries the set; find_glyph
// answers null for a font without it and the mapping answers the
// sentinel sprite - the consumers draw their pre-icon square instead, so
// a fontless or icon-less theme degrades, never breaks). Pure lookup: no
// context, no state, no allocation - the painters row's own contract.
// (The painters fold - stock unless a skin overrides the icon_of row -
// lives in painters.hpp as icon_sprite_of, the one place both types are
// complete.)
[[nodiscard]] inline r2d::Sprite stock_icon_sprite(const ChromeTheme& theme,
                                                   Icon icon) {
    if (theme.font == nullptr) {
        return {};
    }
    const r2d::Font::Glyph* glyph =
        theme.font->find_glyph(icon_codepoint(icon));
    return glyph != nullptr ? glyph->sprite : r2d::Sprite{};
}

// Draws one icon sprite centered in a `size` square at (x, y), pixel-
// snapped like every static painter rect. A sentinel sprite draws
// nothing (the caller's fallback decides what the pre-icon look is).
// White tint keeps the baked coverage's own alpha - the glyph-tint
// convention - so the color rides the packed rgba like every draw.
inline void draw_icon(float x, float y, float size, r2d::Sprite sprite,
                      std::uint32_t color) {
    if (sprite.page == 0 || size <= 0.0f) {
        return;
    }
    const float snapped = std::floor(x + 0.5f);
    const float snapped_y = std::floor(y + 0.5f);
    r2d::draw_sprite(snapped, snapped_y, size, size, color, sprite);
}

}  // namespace ws::ui::gui
