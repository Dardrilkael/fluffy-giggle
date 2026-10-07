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
    std::uint32_t control_on = r2d::rgba(90, 168, 110);  // toggled/checked accent
    std::uint32_t text = r2d::rgba(235, 238, 242);
    std::uint32_t border = r2d::rgba(255, 255, 255, 36);

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
        return t;
    }
};

// The stock look. constexpr, so a custom theme starts from it with
// designated member overrides and stays a compile-time value.
inline constexpr ChromeTheme kDefaultChrome{};

}  // namespace ws::ui::gui
