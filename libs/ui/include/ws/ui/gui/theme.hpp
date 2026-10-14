#pragma once

#include <ws/ui/renderer2d.hpp>

#include <cstdint>

namespace ws::ui::gui {

// The look of the gui: one struct of plain values, no registries and no
// inheritance. Widgets take a const ChromeTheme& and read it inline, so
// a panel can carry its own variant without touching anyone else's
// look. Phase 6 wires colors and metrics; corner_radius is carried but
// only honored once r2d can draw rounded quads (Phase 8).
struct ChromeTheme {
    // Faces, packed 0xRRGGBBAA (r2d::rgba order).
    std::uint32_t panel_bg = r2d::rgba(38, 40, 46);
    std::uint32_t control_bg = r2d::rgba(70, 73, 78);
    std::uint32_t control_hovered = r2d::rgba(92, 96, 102);
    std::uint32_t control_pressed = r2d::rgba(72, 132, 192);
    std::uint32_t control_disabled = r2d::rgba(50, 52, 56);
    std::uint32_t control_on = r2d::rgba(90, 168, 110);  // toggled/checked accent
    std::uint32_t text = r2d::rgba(235, 238, 242);
    std::uint32_t border = r2d::rgba(255, 255, 255, 36);
    // The modal backdrop: a translucent wash drawn over the whole frame
    // at the modal stratum while a modal scope is open - the veil the
    // input rules already enforce, made visible. Not dim() territory:
    // it is its own face, never scaled or dimmed.
    std::uint32_t veil = r2d::rgba(0, 0, 0, 96);

    // Metrics, gui-space pixels.
    float control_height = 24.0f;
    float corner_radius = 4.0f;  // drawn as real corners in Phase 8
    float padding = 12.0f;       // panel content inset
    float spacing = 8.0f;        // the gap between widgets sharing a row
    float scroll_speed = 32.0f;  // px one wheel unit scrolls a scroll panel
    float drag_threshold = 8.0f;  // px a touch may wander from its press
                                  // before a scroll panel calls it a drag -
                                  // inside the window the press stays the
                                  // child's (a tap), past it the scroll owns
                                  // the finger
    float label_char_width = 6.0f;  // the fontless label estimate: px per
                                    // character (the v1 bar placeholder
                                    // and fit_text's no-font measure)
    float label_inset = 2.0f;  // px a fitted label keeps clear of its
                               // control's edge (the slider's right
                               // margin; its fit budget is the width
                               // minus two of these)
    float check_inset = 5.0f;  // the checkbox fill's inset past the box's
                               // corners, max'd with corner_radius so a
                               // round theme cannot let the fill poke
                               // outside the arcs
    float slider_handle_width = 10.0f;  // the slider handle's px
    float scrollbar_thumb_width = 4.0f;  // the scrollbar thumb's px
    float scrollbar_band_width = 12.0f;  // the thumb's grabbable gutter
    float scrollbar_min_thumb = 12.0f;   // the thumb's minimum px height -
                                        // a huge content cannot shrink it
                                        // to nothing
    float disabled_alpha = 0.45f;  // the multiplier a disabled widget dims
                                  // its non-face colors by (label text and
                                  // bars, the checkbox fill, the slider
                                  // fill and handle, the selected tab's
                                  // accent) through dim(); 1.0 is the
                                  // identity. A ratio, not a px metric -
                                  // scaled() carries it as-is

    // The font labels draw with - the one look asset that is a loaded
    // object instead of a value. Null (the default) keeps the v1 label
    // bars, so a fontless theme is the exact pre-font look. The theme
    // does NOT own the font: load it once (r2d::load_font, valid from
    // on_setup()) and keep it alive as long as any theme pointing at it
    // is in use - one Font for the app's lifetime is the intended shape.
    // A second face (a heading weight, say) grows a sibling field here,
    // not a rework of this pointer.
    const r2d::Font* font = nullptr;

    // A copy with every METRIC multiplied - the touch-density move. The
    // shell reports the platform's ui scale (App::ui_scale(): display
    // density / 160 on Android, content scale on desktop); an app that
    // scales its theme, its font bake and its panel rects by the same
    // factor gets a gui that is finger-sized on a phone and unchanged on
    // a 100%-scaling desktop (1.0 is the identity). Faces and the font
    // pointer carry as-is - the caller loads the bigger font.
    constexpr ChromeTheme scaled(float scale) const {
        ChromeTheme t = *this;
        t.control_height = control_height * scale;
        t.corner_radius = corner_radius * scale;
        t.padding = padding * scale;
        t.spacing = spacing * scale;
        t.scroll_speed = scroll_speed * scale;
        t.drag_threshold = drag_threshold * scale;
        t.label_char_width = label_char_width * scale;
        t.label_inset = label_inset * scale;
        t.check_inset = check_inset * scale;
        t.slider_handle_width = slider_handle_width * scale;
        t.scrollbar_thumb_width = scrollbar_thumb_width * scale;
        t.scrollbar_band_width = scrollbar_band_width * scale;
        t.scrollbar_min_thumb = scrollbar_min_thumb * scale;
        return t;
    }
};

// The stock look. constexpr, so a custom theme starts from it with
// designated member overrides and stays a compile-time value.
inline constexpr ChromeTheme kDefaultChrome{};

// Multiplies a packed 0xRRGGBBAA color's alpha channel - the disabled
// look's one helper. control_disabled covers a disabled widget's FACE
// (its idle, hover and pressed faces all collapse to it); every other
// color the widget draws passes through here with the theme's
// disabled_alpha, so the whole widget dims as one and full-strength
// text never rides a dead face. The factor clamps to [0, 1] worth of
// alpha - negative and oversaturated inputs cannot wrap the byte.
constexpr std::uint32_t dim(std::uint32_t color, float factor) {
    float a = static_cast<float>((color >> 24) & 0xFFu) * factor;
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 255.0f) {
        a = 255.0f;
    }
    return (color & 0x00FFFFFFu) | (static_cast<std::uint32_t>(a) << 24);
}

}  // namespace ws::ui::gui
