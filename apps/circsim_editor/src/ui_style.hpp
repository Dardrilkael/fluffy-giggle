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
#include <ws/ui/renderer2d.hpp>

#include <cstdint>

namespace editor::style {

using ws::ui::gui::ChromeTheme;
using ws::ui::r2d::rgba;

// --- the dark chrome: "Night Circuit" -------------------------------------
//
// Layering (dark to light): canvas_bg < bar_bg < panel_bg < control_bg.
// Designated initializers must follow ChromeTheme's declaration order.
inline constexpr ChromeTheme kNightChrome{
    .panel_bg = rgba(22, 25, 38),
    .control_bg = rgba(36, 40, 60),
    .control_hovered = rgba(52, 58, 86),
    .control_pressed = rgba(98, 108, 238),
    .control_disabled = rgba(22, 25, 38),
    .control_on = rgba(118, 126, 255),
    .text_on_accent = rgba(255, 255, 255),
    .accent_hover = rgba(150, 157, 255),
    .text = rgba(231, 234, 246),
    .text_secondary = rgba(156, 163, 188),
    .text_muted = rgba(133, 139, 164),
    .border = rgba(170, 180, 255, 28),
    .border_strong = rgba(170, 180, 255, 58),
    .bar_bg = rgba(14, 16, 26),
    .veil = rgba(6, 8, 18, 150),
    .toast_info = rgba(66, 133, 244),
    .toast_warning = rgba(245, 166, 35),
    .toast_error = rgba(240, 82, 82),
    .shadow = rgba(0, 0, 0, 150),
    .danger = rgba(240, 82, 82),
    .danger_hover = rgba(250, 116, 116),
    .danger_pressed = rgba(214, 64, 64),
    .canvas_bg = rgba(15, 19, 31),
    .grid_minor = rgba(150, 165, 225, 54),
    .grid_major = rgba(150, 165, 225, 120),
    .grid_axis = rgba(118, 126, 255, 80),
    .control_height = 30.0f,
    .corner_radius = 8.0f,
    .radius_control = 6.0f,
    .radius_panel = 10.0f,
    .radius_popup = 10.0f,
    .radius_pill = 14.0f,
    .focus_ring_width = 2.0f,
    .shadow_blur = 18.0f,
    .shadow_offset_y = 6.0f,
    .anim_duration = 0.12f,
};

// --- the light chrome: "Daylight" -----------------------------------------
inline constexpr ChromeTheme kDaylightChrome{
    .panel_bg = rgba(248, 249, 253),
    .control_bg = rgba(230, 233, 246),
    .control_hovered = rgba(213, 219, 240),
    .control_pressed = rgba(55, 48, 163),
    .control_disabled = rgba(243, 244, 250),
    .control_on = rgba(79, 70, 229),
    .text_on_accent = rgba(255, 255, 255),
    .accent_hover = rgba(99, 102, 241),
    .text = rgba(24, 27, 42),
    .text_secondary = rgba(88, 94, 120),
    .text_muted = rgba(116, 121, 142),
    .border = rgba(30, 36, 90, 36),
    .border_strong = rgba(30, 36, 90, 70),
    .bar_bg = rgba(235, 238, 248),
    .veil = rgba(40, 44, 70, 120),
    .toast_info = rgba(37, 99, 235),
    .toast_warning = rgba(217, 119, 6),
    .toast_error = rgba(220, 38, 38),
    .shadow = rgba(30, 34, 70, 70),
    .danger = rgba(200, 40, 52),
    .danger_hover = rgba(222, 72, 82),
    .danger_pressed = rgba(166, 30, 40),
    .canvas_bg = rgba(252, 253, 255),
    .grid_minor = rgba(60, 72, 130, 58),
    .grid_major = rgba(60, 72, 130, 120),
    .grid_axis = rgba(79, 70, 229, 84),
    .control_height = 30.0f,
    .corner_radius = 8.0f,
    .radius_control = 6.0f,
    .radius_panel = 10.0f,
    .radius_popup = 10.0f,
    .radius_pill = 14.0f,
    .focus_ring_width = 2.0f,
    .shadow_blur = 18.0f,
    .shadow_offset_y = 6.0f,
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
