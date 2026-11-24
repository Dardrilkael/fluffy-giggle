#pragma once

// apps/circsim_editor - the editor's visual identity.
//
// Two things live here, both pure data (no canvas, no renderer, no
// platform - the headless suites can include it):
//
//   1. The app's two chromes, kNightChrome and kDaylightChrome: a
//      ChromeTheme each, replacing the library's gray stock themes for
//      this app only (libs/ui and its tests keep kDefaultChrome /
//      kLightChrome untouched). Deep blue-tinted surfaces in three
//      depth layers, a vivid indigo accent, the schematic canvas one
//      step DARKER than the panels so the drawing is the focus, softer
//      borders, rounder corners.
//
//   2. device_color(): one ink per device family, keyed on the
//      symbol's SPICE letter. The schematic, the palette tiles and the
//      refdes labels all read it, so a resistor is the same amber in
//      the Palette, on the canvas and in its label - the way every
//      pro EDA tool color-codes parts so a drawing reads at a glance.
//
// Both chromes are checked by contrast_failures() in the style suite
// (tests/test_ui_style.cpp); every device ink clears 3:1 against its
// own canvas. Change a color here, run the test, and the accessibility
// floor is enforced for you.

#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/renderer2d.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace editor::style {

using ws::ui::gui::ChromeTheme;
using ws::ui::r2d::rgba;

// --- the dark chrome: "Night Circuit" -------------------------------------
//
// The 2026 redesign pass: a restrained engineering-software palette.
// The old chrome leaned on indigo-tinted faces everywhere, shadows and
// 10px radii for a soft "product" look; this one follows the KiCad /
// LTspice / IDE convention - one near-neutral blue-gray ramp for every
// surface, structure through SUBTLE depth steps instead of borders,
// and the accent reserved for state and the one primary action.
//
// The ramp (dark to light): canvas_bg < bar_bg < panel_bg < elevated
// (control_bg). Faces deliberately close: hierarchy comes from WHICH
// layer a thing sits on, not from how different the layers look.
inline constexpr ChromeTheme kNightChrome{
    .panel_bg = rgba(25, 29, 40),        // #191D28 surface
    .control_bg = rgba(34, 40, 56),      // #222838 elevated surface
    .control_hovered = rgba(43, 50, 69),
    .control_pressed = rgba(99, 115, 175),
    .control_disabled = rgba(25, 29, 40),
    .control_on = rgba(112, 132, 235),   // the accent - one step darker
                                         // than #8295FF so a white
                                         // on-accent label clears 3:1
    .text_on_accent = rgba(255, 255, 255),
    .accent_hover = rgba(145, 161, 245),
    .text = rgba(229, 231, 235),        // #E5E7EB primary text
    .text_secondary = rgba(146, 155, 173),  // #929BAD secondary text
    .text_muted = rgba(124, 132, 150),  // the chrome rung - placeholders
    .border = rgba(255, 255, 255, 22),
    .border_strong = rgba(255, 255, 255, 48),
    .bar_bg = rgba(17, 19, 26),          // #11131A app background
    .veil = rgba(8, 10, 16, 170),
    .toast_info = rgba(96, 130, 200),
    .toast_warning = rgba(204, 153, 68),
    .toast_error = rgba(214, 88, 80),
    .shadow = rgba(0, 0, 0, 120),
    .danger = rgba(226, 96, 88),
    .danger_hover = rgba(238, 118, 108),
    .danger_pressed = rgba(198, 76, 68),
    .canvas_bg = rgba(21, 24, 33),       // the drawing surface, one step
                                         // below the panel
    .grid_minor = rgba(255, 255, 255, 38),  // ~15%: texture, not lines
    .grid_major = rgba(255, 255, 255, 92),  // ~36%: every fifth dot
    .grid_axis = rgba(130, 149, 255, 64),
    // Metrics: the density pass. 26px controls (was 30) buy two extra
    // rows of content per panel; 4/3/4 radii (was 8/6/10/14) read as
    // crisp tooling, not soft product cards; no shadows anywhere
    // (shadow_blur 0 - elevation is the surface ramp's own job);
    // 10px panel padding (was 12) tightens the chrome without
    // crowding the content.
    .control_height = 26.0f,
    .corner_radius = 4.0f,
    .radius_control = 3.0f,
    .radius_panel = 4.0f,
    .radius_popup = 5.0f,
    .radius_pill = 8.0f,
    .padding = 10.0f,
    .spacing = 6.0f,
    .focus_ring_width = 1.5f,
    .anim_duration = 0.12f,
};

// --- the light chrome: "Daylight" -----------------------------------------
//
// The same neutral ramp walked bright: app background the palest,
// surfaces one step up, the accent deep enough for white-on-accent
// labels. Metrics identical to the night chrome (density is not a
// dark-mode luxury).
inline constexpr ChromeTheme kDaylightChrome{
    .panel_bg = rgba(246, 247, 250),
    .control_bg = rgba(235, 237, 243),
    .control_hovered = rgba(222, 226, 234),
    .control_pressed = rgba(94, 106, 155),
    .control_disabled = rgba(243, 244, 249),
    .control_on = rgba(93, 106, 190),
    .text_on_accent = rgba(255, 255, 255),
    .accent_hover = rgba(118, 130, 208),
    .text = rgba(23, 26, 33),
    .text_secondary = rgba(96, 102, 118),
    .text_muted = rgba(122, 128, 143),
    .border = rgba(28, 32, 45, 34),
    .border_strong = rgba(28, 32, 45, 64),
    .bar_bg = rgba(236, 238, 244),
    .veil = rgba(40, 44, 70, 120),
    .toast_info = rgba(66, 110, 190),
    .toast_warning = rgba(186, 126, 40),
    .toast_error = rgba(196, 58, 54),
    .shadow = rgba(40, 44, 70, 60),
    .danger = rgba(198, 44, 48),
    .danger_hover = rgba(218, 68, 72),
    .danger_pressed = rgba(168, 34, 38),
    .canvas_bg = rgba(252, 253, 255),
    .grid_minor = rgba(48, 58, 84, 48),
    .grid_major = rgba(48, 58, 84, 105),
    .grid_axis = rgba(93, 106, 190, 80),
    .control_height = 26.0f,
    .corner_radius = 4.0f,
    .radius_control = 3.0f,
    .radius_panel = 4.0f,
    .radius_popup = 5.0f,
    .radius_pill = 8.0f,
    .padding = 10.0f,
    .spacing = 6.0f,
    .focus_ring_width = 1.5f,
    .anim_duration = 0.12f,
};

// The app chrome that replaces a stock one: the shell still decides
// dark vs light (the "shell.theme" setting) and hands back a stock
// theme; the brightness of its panel face picks our matching palette.
// Any theme the shell might hand back that is not one of the two
// stocks still lands on the right side of the dark/light line.
[[nodiscard]] constexpr const ChromeTheme& chrome_for(
    const ChromeTheme& stock) noexcept {
    return ws::ui::gui::is_dark(stock) ? kNightChrome : kDaylightChrome;
}

// --- accent presets ---------------------------------------------------------
//
// The View menu's "Accent Color" picker. Each preset is ONE hue in a
// dark-chrome and a light-chrome variant (the light one walked darker so
// white-on-accent labels keep their contrast). The default, indigo, is
// the palette the two chromes above were tuned around, so choosing it
// changes nothing; every other preset is derived onto the chrome through
// the library's apply_accent, which re-derives the hover/pressed rungs
// and the on-accent label color and keeps the pairs above their floors.
// The style suite sweeps every preset on both chromes through
// contrast_failures - a new preset cannot ship unreadable.
struct AccentPreset {
    std::string_view key;    // the "editor.accent" setting's value
    std::string_view label;  // the menu text
    std::uint32_t dark;      // the accent on the night chrome
    std::uint32_t light;     // the accent on the daylight chrome
};

inline constexpr std::array<AccentPreset, 7> kAccentPresets{{
    {"indigo", "Indigo", rgba(112, 132, 235), rgba(93, 106, 190)},
    {"violet", "Violet", rgba(164, 140, 250), rgba(118, 74, 230)},
    {"sky", "Sky", rgba(84, 180, 250), rgba(14, 122, 190)},
    {"teal", "Teal", rgba(64, 205, 190), rgba(18, 138, 128)},
    {"emerald", "Emerald", rgba(84, 200, 150), rgba(30, 140, 100)},
    {"amber", "Amber", rgba(240, 185, 60), rgba(184, 126, 20)},
    {"rose", "Rose", rgba(246, 122, 141), rgba(198, 52, 88)},
}};

// The preset a setting value names; an unknown or empty value is the
// default (indigo) - a hand-edited settings.json never breaks the chrome.
[[nodiscard]] constexpr const AccentPreset& accent_preset(
    std::string_view key) noexcept {
    for (const AccentPreset& p : kAccentPresets) {
        if (p.key == key) {
            return p;
        }
    }
    return kAccentPresets.front();
}

// The app chrome for a stock theme and an accent key: the dark/light
// palette, re-accented unless the key is the default.
[[nodiscard]] inline ChromeTheme accent_chrome(const ChromeTheme& stock,
                                               std::string_view key) {
    const bool dark = ws::ui::gui::is_dark(stock);
    ChromeTheme theme = dark ? kNightChrome : kDaylightChrome;
    const AccentPreset& preset = accent_preset(key);
    if (&preset != &kAccentPresets.front()) {
        ws::ui::gui::apply_accent(theme, dark ? preset.dark : preset.light);
    }
    return theme;
}

// The "all good" ink (an empty Problems panel, a clean run): the
// inductor green, the one green on each canvas already proven legible
// there, so no new color enters the palette.
[[nodiscard]] constexpr std::uint32_t success_color(bool dark) noexcept {
    return dark ? rgba(74, 222, 128) : rgba(12, 124, 62);
}

// --- device-family ink ------------------------------------------------------
//
// One hue per family, in a dark-canvas and a light-canvas variant (the
// light variants are the same hues walked darker so they keep >= 3:1
// on the near-white canvas). Keyed on SymbolDef::spice_letter:
//
//   R resistor    amber        C capacitor   sky blue
//   L inductor    green        K coupling    green (with its L's)
//   V / I source  rose         D diode, LED  violet
//   Q M J         teal         (BJT, MOSFET, JFET)
//   E F G H B     yellow       (dependent / behavioral sources)
//   S / W         lime         (switches)
//   X             indigo       (op amp, subcircuits)
//   T             fuchsia      (transmission line)
//
// Power and ground symbols (letter 0) and anything unrecognized return
// `fallback`, so they keep the neutral body ink the canvas always had.
[[nodiscard]] constexpr std::uint32_t device_color(
    char spice_letter, bool dark, std::uint32_t fallback) noexcept {
    char c = spice_letter;
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    switch (c) {
        case 'R': return dark ? rgba(255, 176, 66) : rgba(184, 92, 0);
        case 'C': return dark ? rgba(74, 196, 255) : rgba(0, 110, 182);
        case 'L':
        case 'K': return dark ? rgba(74, 222, 128) : rgba(12, 124, 62);
        case 'V':
        case 'I': return dark ? rgba(255, 108, 132) : rgba(196, 28, 68);
        case 'D': return dark ? rgba(194, 136, 255) : rgba(122, 44, 210);
        case 'Q':
        case 'M':
        case 'J': return dark ? rgba(45, 212, 191) : rgba(12, 118, 108);
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case 'B': return dark ? rgba(245, 208, 50) : rgba(140, 100, 0);
        case 'S':
        case 'W': return dark ? rgba(170, 230, 60) : rgba(78, 124, 8);
        case 'X': return dark ? rgba(140, 150, 255) : rgba(70, 60, 210);
        case 'T': return dark ? rgba(242, 124, 222) : rgba(168, 36, 148);
        default: return fallback;
    }
}

}  // namespace editor::style
