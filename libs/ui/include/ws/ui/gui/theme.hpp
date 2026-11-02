#pragma once

#include <ws/ui/renderer2d.hpp>

#include <cstdint>

namespace ws::ui::gui {

// The painter seam's table (painters.hpp defines it; the theme carries
// only the pointer, so no include cycle).
struct WidgetPainters;

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
    std::uint32_t control_on = r2d::rgba(84, 144, 212);  // the accent:
                                   // checked, toggled, selected, live data.
                                   // One step deeper than the pre-P2 blue
                                   // so text_on_accent (a near-white label
                                   // on an accent face) clears its pair.
    // The label color on ACCENT faces - a primary button's text, the
    // checked fill's mark: near-white on both stocks, derived by
    // apply_accent (white when it clears, walked to black when only
    // black can). Checked at the component ratio (3:1), the WCAG
    // 1.4.11 reading an on-accent label sits under - the AA 4.5 the
    // body-text pairs carry is mathematically out of reach for a
    // near-white on any accent that itself clears 3:1 against the
    // dark stock's backgrounds.
    std::uint32_t text_on_accent = r2d::rgba(255, 255, 255);
    // The accent's own hover rung - the toggle/slider handle brightens
    // while the pointer rests on it (control_hovered is the CONTROL
    // face's rung; this is the accent's). Exempt like every hover: the
    // pointer is the indicator. apply_accent derives it.
    std::uint32_t accent_hover = r2d::rgba(112, 168, 228);
    std::uint32_t text = r2d::rgba(228, 231, 238);
    // The quiet rung of text - placeholders, shortcut hints, secondary
    // labels: a real text color (checked 4.5:1 against both
    // backgrounds, like `text`), a step toward the background in
    // lightness. NOT dim(text): a theme that wants its quiet rung
    // bluer or warmer says so here, once.
    std::uint32_t text_secondary = r2d::rgba(150, 156, 170);
    std::uint32_t border = r2d::rgba(255, 255, 255, 26);
    // The strong divider - table rules, active edges, the separator
    // a busy screen needs to read as structure. A step above `border`
    // in the same translucency family; exempt from checks (decorative
    // face, the border's own class).
    std::uint32_t border_strong = r2d::rgba(255, 255, 255, 51);
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
    // The color shadows multiply toward - black at low alpha on the
    // dark stock, a warm gray on the light one. Drawn by the P5
    // shadow primitive against the theme's shadow metrics below;
    // until then it rides as data.
    std::uint32_t shadow = r2d::rgba(0, 0, 0, 110);

    // Metrics, gui-space pixels.
    float control_height = 28.0f;  // the one deliberate stock bump of
                                   // the token pass (26 before): room
                                   // for the text quality pass's
                                   // crisper small glyphs to breathe
    float corner_radius = 5.0f;  // drawn as real corners in Phase 8
    // The radius PER ROLE - each defaults to -1.0f, "corner_radius's
    // own": an app that overrides only corner_radius still restyles
    // every role (the back-compat rule), and an app that wants square
    // controls in a rounded world sets radius_control = 0 alone. Read
    // them through the accessors below (control_radius() et al.),
    // never the raw fields.
    float radius_control = -1.0f;
    float radius_panel = -1.0f;
    float radius_popup = -1.0f;
    float radius_pill = -1.0f;
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
    // The line weights, px. border_width feeds the panel/popup frame
    // stroke (P5 adopts it; today's frames hardcode 1); focus_ring_width
    // the keyboard ring's stroke. scaled() carries them like any px.
    float border_width = 1.0f;
    float focus_ring_width = 1.0f;
    // The shadow metrics, px: blur is the softness, offset_y the drop.
    // Both 0 = the stock draws no shadow - until P5 adopts them, every
    // theme's shadows wait as data (a theme that sets them early just
    // draws them the day P5 lands).
    float shadow_blur = 0.0f;
    float shadow_offset_y = 0.0f;

    // Motion. anim_duration is the seconds an animated value takes to
    // settle (95%) - the P4 helper's one knob; 0 or less is INSTANT,
    // the pre-P4 behavior exactly. anim_enabled is the master switch
    // (false = every traveler jumps, byte-identical to a no-animation
    // build) so an app can offer reduced-motion without losing its
    // configured durations. Timings and switches: scaled() carries
    // them as-is, like tooltip_delay.
    float anim_duration = 0.10f;
    bool anim_enabled = true;

    // The font labels draw with - the one look asset that is a loaded
    // object instead of a value. Null (the default) keeps the v1 label
    // bars, so a fontless theme is the exact pre-font look. The theme
    // does NOT own the font: load it once (r2d::load_font, valid from
    // on_setup()) and keep it alive as long as any theme pointing at it
    // is in use - one Font for the app's lifetime is the intended shape.
    // A second face (a heading weight, say) grows a sibling field here,
    // not a rework of this pointer.
    const r2d::Font* font = nullptr;

    // The strong weight - headings, the selected tab, a dialog's
    // title. Null (the default) falls back to `font` everywhere it is
    // read: a one-font theme stays one font, and a two-weight look is
    // load_default_font(semibold, ...) plus this pointer (see P1's
    // bundled Inter). Same borrowing contract as `font` - the theme
    // owns nothing.
    const r2d::Font* font_strong = nullptr;

    // The painter table (painters.hpp): null = stock (every composed
    // widget calls its own file's draw, the library's built-in look); a
    // skin installs a WidgetPainters here and every row it filled
    // follows it. The theme POINTS, it owns nothing - the table must
    // outlive every theme carrying it (the font pointers' own rule),
    // and scaled() carries it as-is (a skin is not a metric).
    const WidgetPainters* painters = nullptr;

    // The role radii, resolved: -1 (the default) inherits
    // corner_radius, so the back-compat rule holds - one number
    // restyles everything, four numbers restyle one role each.
    // constexpr, like the whole struct.
    [[nodiscard]] constexpr float control_radius() const {
        return radius_control >= 0.0f ? radius_control : corner_radius;
    }
    [[nodiscard]] constexpr float panel_radius() const {
        return radius_panel >= 0.0f ? radius_panel : corner_radius;
    }
    [[nodiscard]] constexpr float popup_radius() const {
        return radius_popup >= 0.0f ? radius_popup : corner_radius;
    }
    [[nodiscard]] constexpr float pill_radius() const {
        return radius_pill >= 0.0f ? radius_pill : corner_radius;
    }

    // A copy with every px METRIC multiplied - the touch-density move.
    // The shell reports the platform's ui scale (App::ui_scale(): display
    // density / 160 on Android, content scale on desktop); an app that
    // scales its theme, its font bake and its panel rects by the same
    // factor gets a gui that is finger-sized on a phone and unchanged on
    // a 100%-scaling desktop (1.0 is the identity). Faces and the font
    // pointers carry as-is - the caller loads the bigger font - and so do
    // the timing metrics, the motion switch, disabled_alpha and the
    // radius sentinels (their fields say why).
    constexpr ChromeTheme scaled(float scale) const {
        ChromeTheme t = *this;
        t.control_height = control_height * scale;
        t.corner_radius = corner_radius * scale;
        t.radius_control =
            radius_control < 0.0f ? -1.0f : radius_control * scale;
        t.radius_panel = radius_panel < 0.0f ? -1.0f : radius_panel * scale;
        t.radius_popup = radius_popup < 0.0f ? -1.0f : radius_popup * scale;
        t.radius_pill = radius_pill < 0.0f ? -1.0f : radius_pill * scale;
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
        t.border_width = border_width * scale;
        t.focus_ring_width = focus_ring_width * scale;
        t.shadow_blur = shadow_blur * scale;
        t.shadow_offset_y = shadow_offset_y * scale;
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
    .text_on_accent = r2d::rgba(255, 255, 255),
    .accent_hover = r2d::rgba(70, 150, 224),
    .text = r2d::rgba(28, 31, 38),
    .text_secondary = r2d::rgba(94, 98, 108),
    .border = r2d::rgba(0, 0, 0, 40),
    .border_strong = r2d::rgba(0, 0, 0, 70),
    .bar_bg = r2d::rgba(232, 233, 238),
    .veil = r2d::rgba(96, 96, 96, 110),
    .shadow = r2d::rgba(50, 50, 55, 70),
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

// The brightness pick the font bake follows (the P1b story): the
// rec.709 luma of a face, on the packed 0xRRGGBBAA order. The stock
// dark chrome's light-on-dark text wants the boldening bake pair
// (FontBakeOptions::dark_chrome()), the light chrome's dark-on-light
// text the near-identity one (light_chrome()) - an app reads this
// against its active chrome's panel_bg (or passes the ChromeTheme
// overload) and re-bakes when the answer flips. constexpr, so the
// stock constants answer at compile time.
[[nodiscard]] constexpr bool is_dark(std::uint32_t face) noexcept {
    const std::uint32_t r = (face >> 24) & 0xFFu;
    const std::uint32_t g = (face >> 16) & 0xFFu;
    const std::uint32_t b = (face >> 8) & 0xFFu;
    return (299u * r + 587u * g + 114u * b) < 128000u;
}

[[nodiscard]] constexpr bool is_dark(const ChromeTheme& theme) noexcept {
    return is_dark(theme.panel_bg);
}

}  // namespace ws::ui::gui
