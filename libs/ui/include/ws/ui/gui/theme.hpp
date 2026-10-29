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
    // Faces, packed 0xRRGGBBAA (r2d::rgba order). Three depth layers by
    // design: bar_bg (the chrome strips) is a step away from panel_bg
    // (the surfaces), and control_bg a step above that (the widgets) -
    // the VS Code layering, so the frame reads as frame and the
    // content as content instead of one flat gray void.
    std::uint32_t panel_bg = r2d::rgba(33, 35, 41);
    std::uint32_t control_bg = r2d::rgba(45, 48, 56);
    std::uint32_t control_hovered = r2d::rgba(62, 66, 77);
    std::uint32_t control_pressed = r2d::rgba(80, 128, 190);
    std::uint32_t control_disabled = r2d::rgba(42, 44, 50);
    std::uint32_t control_on = r2d::rgba(96, 156, 224);  // the accent:
                                   // checked, toggled, selected, live data
    std::uint32_t text = r2d::rgba(228, 231, 238);
    std::uint32_t border = r2d::rgba(255, 255, 255, 26);
    // The chrome strips - the menu bar, the toolbar, the tab strips,
    // the status bar, the dock splitters. Darker than panel_bg on the
    // dark theme (and a grayer step on the light one), the same role
    // a window title bar plays: the frame the surfaces sit in.
    std::uint32_t bar_bg = r2d::rgba(23, 25, 30);
    // The modal backdrop: a translucent wash drawn over the whole frame
    // at the modal stratum while a modal scope is open - the veil the
    // input rules already enforce, made visible. Not dim() territory:
    // it is its own face, never scaled or dimmed.
    std::uint32_t veil = r2d::rgba(0, 0, 0, 96);
    // The toast severities: the accent a toast wears with its text -
    // the action button's face and any severity mark it draws. The
    // toast body itself reuses panel_bg + border, chrome like any
    // panel; the severities are saturated enough to read on both stock
    // themes.
    std::uint32_t toast_info = r2d::rgba(72, 132, 192);
    std::uint32_t toast_warning = r2d::rgba(200, 150, 60);
    std::uint32_t toast_error = r2d::rgba(190, 80, 70);

    // Metrics, gui-space pixels.
    float control_height = 26.0f;
    float corner_radius = 5.0f;  // drawn as real corners in Phase 8
    float padding = 12.0f;       // panel content inset
    float spacing = 8.0f;        // the gap between widgets sharing a row
    float scroll_speed = 32.0f;  // px one wheel unit scrolls a scroll panel
    float drag_threshold = 8.0f;  // px a touch may wander from its press
                                  // before a scroll panel calls it a drag -
                                  // inside the window the press stays the
                                  // child's (a tap), past it the scroll owns
                                  // the finger
    // Timings, SECONDS - not px. scaled() carries them as-is, exactly
    // like disabled_alpha: a denser screen does not make a user
    // faster, and a hover delay is a hover delay at any dpi.
    float tooltip_delay = 0.4f;  // s the pointer must rest on a widget
                                // before tooltip_delayed opens its
                                // tooltip
    float long_press_time = 0.5f;  // s a touch must hold a region,
                                  // without wandering past
                                  // drag_threshold, before it counts as
                                  // a right-click (the context menu)
    float menu_open_delay = 0.25f;  // s the mouse must rest on a menu
                                   // item before its submenu opens;
                                   // touch skips the delay - a tap
                                   // toggles
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

    // A copy with every px METRIC multiplied - the touch-density move.
    // The shell reports the platform's ui scale (App::ui_scale(): display
    // density / 160 on Android, content scale on desktop); an app that
    // scales its theme, its font bake and its panel rects by the same
    // factor gets a gui that is finger-sized on a phone and unchanged on
    // a 100%-scaling desktop (1.0 is the identity). Faces and the font
    // pointer carry as-is - the caller loads the bigger font - and so do
    // the timing metrics and disabled_alpha (their fields say why).
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
// designated member overrides and stays a compile-time value. Both
// stock themes pass the gui/theme_color.hpp contrast checks
// (contrast_failures() is empty on each); the faces are picked, not
// derived, but they answer to the same rules.
inline constexpr ChromeTheme kDefaultChrome{};

// The light stock theme - same metrics, a daylight face set: the dark
// palette with every color flipped (pale chrome, dark text, a gray
// veil instead of a black one). Designated initializers (the C++20
// spelling, one name per face), so the toast severities keep their
// defaults and the metrics and font stay exactly what kDefaultChrome{}
// leaves them - scale it and install a font the same way.
inline constexpr ChromeTheme kLightChrome{
    .panel_bg = r2d::rgba(245, 245, 248),
    .control_bg = r2d::rgba(222, 224, 230),
    .control_hovered = r2d::rgba(202, 205, 214),
    .control_pressed = r2d::rgba(62, 109, 178),
    .control_disabled = r2d::rgba(228, 229, 233),
    .control_on = r2d::rgba(38, 122, 203),
    .text = r2d::rgba(28, 31, 38),
    .border = r2d::rgba(0, 0, 0, 40),
    .bar_bg = r2d::rgba(232, 233, 238),
    .veil = r2d::rgba(96, 96, 96, 110),
};

// Multiplies a packed 0xRRGGBBAA color's alpha channel - the alpha is
// the LOW byte, where r2d::rgba puts it and the batch reader looks
// for it. (The M10 original scaled the TOP byte - red - by mistake,
// and its test pinned that; the stage-10 color tests caught it.) The
// disabled look's one helper. control_disabled covers a disabled
// widget's FACE (its idle, hover and pressed faces all collapse to
// it); every other color the widget draws passes through here with
// the theme's disabled_alpha, so the whole widget dims as one and
// full-strength text never rides a dead face. The factor clamps to
// [0, 1] worth of alpha - negative and oversaturated inputs cannot
// wrap the byte.
constexpr std::uint32_t dim(std::uint32_t color, float factor) {
    float a = static_cast<float>(color & 0xFFu) * factor;
    if (a < 0.0f) {
        a = 0.0f;
    } else if (a > 255.0f) {
        a = 255.0f;
    }
    return (color & 0xFFFFFF00u) | static_cast<std::uint32_t>(a);
}

}  // namespace ws::ui::gui
